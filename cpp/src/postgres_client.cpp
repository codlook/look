#include "look/postgres_client.h"
#include "look/pg_parse.h"   // saf pg_parse_error (fuzz/test ile paylasilan tek tanim)
#include "look/format.h"     // look_format_double (dilin tek double formati)
#include <cstring>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <random>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <netinet/tcp.h>
  #include <fstream>
  // DB-TLS: PostgreSQL SSLRequest → TLS upgrade (Linux; Windows OpenSSL'siz derlenir).
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#endif

namespace look {

// SCRAM-SHA-256 client nonce'u KRİPTOGRAFİK olmalı (RFC 5802 §5.1: nonce
// unpredictable olmalı). std::mt19937 tahmin edilebilir (32-bit random_device
// seed) → CSPRNG değil. Fail-closed güvenli kaynak kullan.
static void pg_secure_random(uint8_t* buf, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("PostgreSQL SCRAM: CSPRNG erişilemedi (BCryptGenRandom)");
#else
    std::ifstream u("/dev/urandom", std::ios::binary);
    u.read(reinterpret_cast<char*>(buf), (std::streamsize)n);
    if (!u || (size_t)u.gcount() != n)
        throw std::runtime_error("PostgreSQL SCRAM: CSPRNG erişilemedi (/dev/urandom)");
#endif
}

#ifndef _WIN32
// ── DB-TLS: client SSL_CTX ───────────────────────────────────────────────────
// PostgreSQL'de TLS YENİ (kırılacak kullanıcı yok) → per-SSL verify DOĞRU varsayılanla
// (SSL_VERIFY_PEER + SSL_set1_host) açılır. CTX yalnız trust store'u yükler; verify modu
// bağlantı başına set edilir (mysql_client:601 deseni, :55'in VERIFY_NONE'ı DEĞİL).
static SSL_CTX* pg_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c) SSL_CTX_set_default_verify_paths(c);   // sistem CA'ları (verify modu için)
        return c;
    }();
    return ctx;
}
#endif

// ── MD5 (RFC 1321) ────────────────────────────────────────────────────────────

static const uint32_t MD5_T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

std::array<uint8_t,16> PostgresClient::md5_raw(const uint8_t* data, size_t len) {
    uint32_t s[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = (uint64_t)len * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; i++) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    auto rot = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t m[16];
        for (int j = 0; j < 16; j++)
            m[j] = msg[i+j*4] | ((uint32_t)msg[i+j*4+1]<<8) |
                   ((uint32_t)msg[i+j*4+2]<<16) | ((uint32_t)msg[i+j*4+3]<<24);
        uint32_t a=s[0], b=s[1], c=s[2], d=s[3];
        for (int j = 0; j < 64; j++) {
            uint32_t f; int g;
            if      (j < 16) { f = (b&c)|(~b&d);  g = j; }
            else if (j < 32) { f = (d&b)|(~d&c);  g = (5*j+1)%16; }
            else if (j < 48) { f = b^c^d;          g = (3*j+5)%16; }
            else             { f = c^(b|(~d));     g = (7*j)%16; }
            f = f + a + MD5_T[j] + m[g];
            a = d; d = c; c = b;
            b = b + rot(f, MD5_S[j]);
        }
        s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
    }

    std::array<uint8_t,16> r{};
    for (int i = 0; i < 4; i++) {
        r[i*4]=(s[i])&0xFF; r[i*4+1]=(s[i]>>8)&0xFF;
        r[i*4+2]=(s[i]>>16)&0xFF; r[i*4+3]=(s[i]>>24)&0xFF;
    }
    return r;
}

std::string PostgresClient::md5_hex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    auto h = md5_raw(data, len);
    std::string out;
    out.reserve(32);
    for (auto b : h) { out += hex[b>>4]; out += hex[b&0xF]; }
    return out;
}

std::string PostgresClient::pg_md5_password(const std::string& password,
                                              const std::string& user,
                                              const uint8_t salt[4]) {
    // inner = md5(password + user)
    std::string inner = password + user;
    std::string inner_hex = md5_hex((const uint8_t*)inner.data(), inner.size());
    // outer = md5(inner_hex + salt)
    std::string outer = inner_hex;
    outer.append((const char*)salt, 4);
    return "md5" + md5_hex((const uint8_t*)outer.data(), outer.size());
}

// ── SHA-256 (FIPS 180-4) ──────────────────────────────────────────────────────

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

std::array<uint8_t,32> PostgresClient::sha256(const uint8_t* data, size_t len) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = (uint64_t)len * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[64];
        for (int j = 0; j < 16; j++)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 64; j++) {
            uint32_t s0 = rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
            uint32_t s1 = rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
            w[j] = w[j-16] + s0 + w[j-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int j = 0; j < 64; j++) {
            uint32_t S1   = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch   = (e&f)^(~e&g);
            uint32_t tmp1 = hh + S1 + ch + SHA256_K[j] + w[j];
            uint32_t S0   = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj  = (a&b)^(a&c)^(b&c);
            uint32_t tmp2 = S0 + maj;
            hh=g; g=f; f=e; e=d+tmp1; d=c; c=b; b=a; a=tmp1+tmp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::array<uint8_t,32> out{};
    for (int i = 0; i < 8; i++) {
        out[i*4]=(h[i]>>24)&0xFF; out[i*4+1]=(h[i]>>16)&0xFF;
        out[i*4+2]=(h[i]>>8)&0xFF; out[i*4+3]=h[i]&0xFF;
    }
    return out;
}

std::array<uint8_t,32> PostgresClient::hmac_sha256(const uint8_t* key, size_t klen,
                                                     const uint8_t* msg, size_t mlen) {
    const int BLOCK = 64;
    uint8_t k[64] = {};
    if (klen > (size_t)BLOCK) {
        auto kh = sha256(key, klen);
        memcpy(k, kh.data(), 32);
    } else {
        memcpy(k, key, klen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < BLOCK; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }

    std::vector<uint8_t> inner(BLOCK + mlen);
    memcpy(inner.data(), ipad, BLOCK);
    memcpy(inner.data() + BLOCK, msg, mlen);
    auto inner_hash = sha256(inner.data(), inner.size());

    std::vector<uint8_t> outer(BLOCK + 32);
    memcpy(outer.data(), opad, BLOCK);
    memcpy(outer.data() + BLOCK, inner_hash.data(), 32);
    return sha256(outer.data(), outer.size());
}

std::array<uint8_t,32> PostgresClient::pbkdf2_sha256(const std::string& pass,
                                                       const uint8_t* salt, size_t slen, int iters) {
    // PRF = HMAC-SHA-256, block index = 1 (32 bytes output — one block yeterli)
    const uint8_t* pk  = (const uint8_t*)pass.data();
    size_t         pklen = pass.size();

    std::vector<uint8_t> s1(slen + 4);
    memcpy(s1.data(), salt, slen);
    s1[slen]=0; s1[slen+1]=0; s1[slen+2]=0; s1[slen+3]=1; // INT(1) big-endian

    auto u = hmac_sha256(pk, pklen, s1.data(), s1.size());
    auto out = u;
    for (int i = 1; i < iters; i++) {
        u = hmac_sha256(pk, pklen, u.data(), u.size());
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
    return out;
}

static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string PostgresClient::base64_enc(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i+1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) v |= (uint32_t)data[i+2];
        out += B64_CHARS[(v>>18)&0x3F];
        out += B64_CHARS[(v>>12)&0x3F];
        out += (i+1 < len) ? B64_CHARS[(v>>6)&0x3F] : '=';
        out += (i+2 < len) ? B64_CHARS[v&0x3F]      : '=';
    }
    return out;
}

std::vector<uint8_t> PostgresClient::base64_dec(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    uint32_t v = 0; int bits = 0;
    for (char c : s) {
        int d = val(c);
        if (d < 0) continue;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(v >> bits)); v &= (1u<<bits)-1; }
    }
    return out;
}

// ── SCRAM-SHA-256 (RFC 7677) ──────────────────────────────────────────────────

void PostgresClient::do_scram_sha256(const std::vector<uint8_t>& /*mechanisms_body*/) {
    // 18-byte CSPRNG nonce (RFC 5802: unpredictable olmalı — mt19937 değil)
    std::vector<uint8_t> nonce_raw(18);
    pg_secure_random(nonce_raw.data(), nonce_raw.size());
    std::string c_nonce = base64_enc(nonce_raw.data(), nonce_raw.size());

    // client-first-message-bare
    std::string cfm_bare = "n=,r=" + c_nonce;
    // client-first-message (with gs2-header "n,,")
    std::string cfm = "n,," + cfm_bare;

    // AuthenticationSASLInitialResponse
    {
        std::string mech = "SCRAM-SHA-256";
        std::vector<uint8_t> body;
        body.insert(body.end(), mech.begin(), mech.end());
        body.push_back('\0');
        uint32_t msg_len = (uint32_t)cfm.size();
        body.push_back((msg_len>>24)&0xFF); body.push_back((msg_len>>16)&0xFF);
        body.push_back((msg_len>>8)&0xFF);  body.push_back(msg_len&0xFF);
        body.insert(body.end(), cfm.begin(), cfm.end());
        send_message('p', body);
    }

    // AuthenticationSASLContinue (R, auth_type=11)
    auto msg = read_message();
    if (msg.type != 'R' || msg.body.size() < 4)
        throw std::runtime_error("db postgres: expected SASLContinue");
    uint32_t atype = read_u32_be(msg.body.data());
    if (atype != 11)
        throw std::runtime_error("db postgres: expected auth_type 11 (SASLContinue), got " + std::to_string(atype));

    std::string sfm(msg.body.begin() + 4, msg.body.end());

    // Parse server-first: r=<nonce>,s=<salt>,i=<iter>
    std::string s_nonce, s_salt_b64, s_salt_str;
    int iter = 0;
    {
        std::istringstream ss(sfm);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.size() > 2 && tok[0]=='r' && tok[1]=='=') s_nonce   = tok.substr(2);
            if (tok.size() > 2 && tok[0]=='s' && tok[1]=='=') s_salt_b64= tok.substr(2);
            if (tok.size() > 2 && tok[0]=='i' && tok[1]=='=') {
                // SCRAM iter server'dan gelir — bozuk değer → 0, aşağıda iter<=0 reddedilir
                try { iter = std::stoi(tok.substr(2)); } catch (...) { iter = 0; }
            }
        }
    }
    if (s_nonce.empty() || s_salt_b64.empty() || iter <= 0)
        throw std::runtime_error("db postgres: malformed SASLContinue");
    // Verify client nonce is prefix
    if (s_nonce.substr(0, c_nonce.size()) != c_nonce)
        throw std::runtime_error("db postgres: SCRAM nonce mismatch");

    auto salt_bytes = base64_dec(s_salt_b64);

    // client-final-message-without-proof
    std::string gs2_b64 = base64_enc((const uint8_t*)"n,,", 3);
    std::string cfm_no_proof = "c=" + gs2_b64 + ",r=" + s_nonce;

    // SaltedPassword = PBKDF2(password, salt, iter)
    auto salted_pwd = pbkdf2_sha256(password_, salt_bytes.data(), salt_bytes.size(), iter);

    // ClientKey = HMAC(SaltedPassword, "Client Key")
    const std::string ck_str = "Client Key";
    auto client_key = hmac_sha256(salted_pwd.data(), salted_pwd.size(),
                                   (const uint8_t*)ck_str.data(), ck_str.size());
    // StoredKey = SHA256(ClientKey)
    auto stored_key = sha256(client_key.data(), client_key.size());

    // AuthMessage = client-first-message-bare + "," + server-first + "," + client-final-without-proof
    std::string auth_msg = cfm_bare + "," + sfm + "," + cfm_no_proof;

    // ClientSignature = HMAC(StoredKey, AuthMessage)
    auto client_sig = hmac_sha256(stored_key.data(), stored_key.size(),
                                   (const uint8_t*)auth_msg.data(), auth_msg.size());
    // ClientProof = ClientKey XOR ClientSignature
    std::array<uint8_t,32> client_proof{};
    for (int i = 0; i < 32; i++) client_proof[i] = client_key[i] ^ client_sig[i];

    // ServerKey = HMAC(SaltedPassword, "Server Key")
    const std::string sk_str = "Server Key";
    auto server_key = hmac_sha256(salted_pwd.data(), salted_pwd.size(),
                                   (const uint8_t*)sk_str.data(), sk_str.size());
    // ExpectedServerSignature = HMAC(ServerKey, AuthMessage)
    auto expected_server_sig = hmac_sha256(server_key.data(), server_key.size(),
                                            (const uint8_t*)auth_msg.data(), auth_msg.size());

    std::string proof_b64 = base64_enc(client_proof.data(), client_proof.size());
    std::string cfm_final = cfm_no_proof + ",p=" + proof_b64;

    // SASLResponse
    {
        std::vector<uint8_t> body(cfm_final.begin(), cfm_final.end());
        send_message('p', body);
    }

    // AuthenticationSASLFinal (R, auth_type=12)
    auto msg2 = read_message();
    if (msg2.type == 'E') throw std::runtime_error("db postgres: " + pg_parse_error(msg2.body));
    if (msg2.type != 'R' || msg2.body.size() < 4)
        throw std::runtime_error("db postgres: expected SASLFinal");
    uint32_t atype2 = read_u32_be(msg2.body.data());
    if (atype2 != 12)
        throw std::runtime_error("db postgres: expected auth_type 12 (SASLFinal)");

    // Verify server signature
    std::string sfinal(msg2.body.begin() + 4, msg2.body.end());
    std::string server_sig_b64;
    {
        std::istringstream ss(sfinal);
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (tok.size() > 2 && tok[0]=='v' && tok[1]=='=') server_sig_b64 = tok.substr(2);
    }
    auto got_server_sig = base64_dec(server_sig_b64);
    if (got_server_sig.size() != 32 ||
        memcmp(got_server_sig.data(), expected_server_sig.data(), 32) != 0)
        throw std::runtime_error("db postgres: SCRAM server signature verification failed");
}

// ── Big-endian helpers ────────────────────────────────────────────────────────

uint32_t PostgresClient::read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
int32_t PostgresClient::read_i32_be(const uint8_t* p) {
    return (int32_t)read_u32_be(p);
}
uint16_t PostgresClient::read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint32_t)p[0]<<8)|(uint32_t)p[1]);
}
void PostgresClient::write_u32_be(uint8_t* p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

// ── OID → LOOK type code ──────────────────────────────────────────────────────

uint8_t PostgresClient::oid_to_type(int32_t oid) {
    // INTEGER türleri
    if (oid == 20 || oid == 21 || oid == 23 || oid == 26 || oid == 16000)
        return pg_type::INT;
    // FLOAT / NUMERIC türleri
    if (oid == 700 || oid == 701 || oid == 1700)
        return pg_type::FLOAT;
    // BOOL
    if (oid == 16)
        return pg_type::BOOL;
    // NULL ve bilinmeyenler → string
    return 0xFE;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

PostgresClient::PostgresClient() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) == 0)
        wsock_init_ = true;
#endif
}

PostgresClient::~PostgresClient() {
    disconnect();
#ifdef _WIN32
    if (wsock_init_) WSACleanup();
#endif
}

void PostgresClient::disconnect() {
    if (sock_ != PG_SOCK_INVALID) {
        // Terminate mesajı gönder (best effort) — TLS aktifse send_bytes SSL üstünden yollar
        try {
            uint8_t msg[5] = {'X', 0, 0, 0, 4};
            send_bytes(msg, 5);
        } catch (...) {}
#ifndef _WIN32
        if (ssl_) {
            SSL* s = (SSL*)ssl_;
            SSL_shutdown(s);
            SSL_free(s);
            ssl_ = nullptr;
        }
#endif
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
    }
}

// ── Socket I/O ────────────────────────────────────────────────────────────────

bool PostgresClient::recv_bytes(uint8_t* buf, size_t len) {
    size_t received = 0;
#ifndef _WIN32
    if (ssl_) {   // TLS aktif → SSL_read (bloklayan)
        SSL* s = (SSL*)ssl_;
        while (received < len) {
            int r = SSL_read(s, buf + received, (int)(len - received));
            if (r <= 0) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; return false; }
            received += r;
        }
        return true;
    }
#endif
    while (received < len) {
        int r = recv(sock_, (char*)(buf + received), (int)(len - received), 0);
        if (r <= 0) {
            pg_close_sock(sock_);
            sock_ = PG_SOCK_INVALID;
            return false;
        }
        received += r;
    }
    return true;
}

// İstek SUNUCUYA ULAŞMADAN kopmayı ("at-most-once güvenli") yanıt-okuma
// aşamasındaki kopmadan ayıran özel tip. query()/execute() YALNIZCA bu tipte
// retry eder — gönderilemeyen istek sunucuda işlenMEMİŞtir, tekrar güvenlidir.
// Yanıt okunurken kopma bu tipte DEĞİLdir → retry edilmez (sunucu INSERT'i
// işlemiş olabilir; kör retry sessiz çift-yazma üretir — 53. bug, sahte-sunucu
// "commit_kopar" ile kanıtlandı).
struct PgSendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void PostgresClient::send_bytes(const uint8_t* buf, size_t len) {
    size_t sent = 0;
#ifndef _WIN32
    if (ssl_) {   // TLS aktif → SSL_write
        SSL* s = (SSL*)ssl_;
        while (sent < len) {
            int r = SSL_write(s, buf + sent, (int)(len - sent));
            if (r <= 0) {
                pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
                throw PgSendError("db postgres: TLS send failed — connection lost");
            }
            sent += r;
        }
        return;
    }
#endif
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), 0);
        if (r <= 0) {
            // read yollariyla SIMETRIK: gonderim kopmasinda soketi kapat ve sock_'u
            // INVALID yap ki "kapali ama gecerli gorunen fd" havuza donup sonraki
            // sorguda zehirlemesin. (query()/execute() basindaki
            // `if sock_==INVALID → do_connect()` bunu otomatik yakalar.) Analizci
            // kaynak denetiminde bu asimetriyi buldu; gozlemlenebilir bug
            // uretilemedi — PgSendError retry'i kurtarilabilir vakalari zaten
            // kurtariyor — ama simetri savunma-derinligi.
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw PgSendError("db postgres: send failed — connection lost");
        }
        sent += r;
    }
}

void PostgresClient::set_socket_timeout(int ms) {
    if (sock_ == PG_SOCK_INVALID) return;
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ── Message I/O ───────────────────────────────────────────────────────────────

// Backend → type(1) + len(4, len içinde kendisi var ama type yok) + body
PostgresClient::PgMsg PostgresClient::read_message() {
    uint8_t type_byte;
    if (!recv_bytes(&type_byte, 1))
        throw std::runtime_error("db postgres: connection lost");
    uint8_t lb[4];
    if (!recv_bytes(lb, 4))
        throw std::runtime_error("db postgres: connection lost reading length");
    uint32_t len = read_u32_be(lb);
    if (len < 4) throw std::runtime_error("db postgres: invalid message length");
    size_t body_len = len - 4;
    // Sunucu-kontrollü len ~4GB'a kadar olabilir → std::vector(body_len) sınırsız
    // allocation → OOM/crash (kötü niyetli/MITM sunucu, TLS yok). Makul üst sınır
    // (varsayılan 256MB, LOOK_PG_MAX_MSG ile ayarlanır).
    static const size_t PG_MAX_MSG = []() -> size_t {
        const char* e = std::getenv("LOOK_PG_MAX_MSG");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return 256ull * 1024 * 1024;
    }();
    if (body_len > PG_MAX_MSG)
        throw std::runtime_error("db postgres: mesaj boyutu sınırı aşıldı (LOOK_PG_MAX_MSG)");
    std::vector<uint8_t> body(body_len);
    if (body_len > 0 && !recv_bytes(body.data(), body_len))
        throw std::runtime_error("db postgres: connection lost reading body");
    return PgMsg{(char)type_byte, std::move(body)};
}

// Frontend → type(1) + len(4, len içinde kendisi var) + body
void PostgresClient::send_message(char type, const std::vector<uint8_t>& body) {
    uint32_t len = 4 + (uint32_t)body.size();
    uint8_t hdr[5];
    hdr[0] = (uint8_t)type;
    write_u32_be(hdr + 1, len);
    send_bytes(hdr, 5);
    if (!body.empty()) send_bytes(body.data(), body.size());
}

// ── Connect ───────────────────────────────────────────────────────────────────

void PostgresClient::connect(const std::string& host, int port,
                              const std::string& user, const std::string& password,
                              const std::string& database) {
    host_ = host; port_ = port; user_ = user;
    password_ = password; database_ = database;
    do_connect();
}

void PostgresClient::do_connect() {
    if (sock_ != PG_SOCK_INVALID) {
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
    }

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == PG_SOCK_INVALID)
        throw std::runtime_error("db postgres: socket creation failed");

    // Non-blocking connect
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port_);

    struct addrinfo* res = nullptr;
    struct addrinfo  hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: cannot resolve host: " + host_);
    }
    addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    ::connect(sock_, (struct sockaddr*)&addr, sizeof(addr));

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock_, &wfds);
    struct timeval tv;
    tv.tv_sec  = connect_timeout_ms_ / 1000;
    tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;
#ifdef _WIN32
    int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
    int sel = select((int)sock_ + 1, nullptr, &wfds, nullptr, &tv);
#endif
    if (sel <= 0) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: timeout connecting to " + host_ + ":" + std::to_string(port_));
    }

    int err = 0;
#ifdef _WIN32
    int elen = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
#else
    socklen_t elen = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif
    if (err != 0) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: cannot connect to " + host_ + ":" + std::to_string(port_));
    }

    // Blocking moduna geri al
#ifdef _WIN32
    u_long bmode = 0;
    ioctlsocket(sock_, FIONBIO, &bmode);
#else
    int bflags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, bflags & ~O_NONBLOCK);
#endif

    // Nagle algoritmasını kapat — küçük paketlerde 40ms gecikmeyi önler
    {
        int nd = 1;
#ifdef _WIN32
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
#else
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
#endif
    }

    set_socket_timeout(query_timeout_ms_);

#ifndef _WIN32
    if (tls_) {
        // ── SSLRequest → TLS upgrade ─────────────────────────────────────────
        // SSLRequest (len=8, code=80877103=0x04D2162F) → sunucu 1 bayt: 'S' kabul / 'N' yok.
        // 'N' ise SESSİZCE DÜZ METNE DÜŞME — TLS'i hiç eklememekten kötü olur (kullanıcı
        // şifreli sanır). tls_ istendiyse temiz hata ver.
        uint8_t sslreq[8] = {0,0,0,8, 0x04,0xD2,0x16,0x2F};
        send_bytes(sslreq, 8);   // ssl_ henüz yok → plaintext
        uint8_t resp = 0;
        if (!recv_bytes(&resp, 1)) {
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error("db postgres: SSLRequest yanıtı okunamadı");
        }
        if (resp != 'S') {
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error(std::string("db postgres: sunucu TLS desteklemiyor "
                "(SSLRequest yanıtı '") + (char)resp + "') — düz metne DÜŞÜLMEDİ. "
                "Şifresiz bağlanmak istiyorsanız postgres:// kullanın.");
        }
        SSL_CTX* ctx = pg_ssl_ctx();
        SSL* s = ctx ? SSL_new(ctx) : nullptr;
        if (!s) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
                  throw std::runtime_error("db postgres: SSL_new başarısız (TLS)"); }
        SSL_set_fd(s, (int)sock_);
        SSL_set_tlsext_host_name(s, host_.c_str());   // SNI
        if (tls_verify_) {
            // Sertifika zinciri + HOSTNAME doğrula (MITM'e karşı). SSL_set1_host başarısızsa
            // hostname kontrolü sessizce kapanır → "geçerli CA + yanlış host" MITM'i açılır → reddet.
            SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
            if (SSL_set1_host(s, host_.c_str()) != 1) {
                SSL_free(s); pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
                throw std::runtime_error("db postgres: TLS hostname doğrulaması kurulamadı");
            }
        }
        if (SSL_connect(s) != 1) {
            unsigned long e = ERR_get_error();
            char eb[256]; ERR_error_string_n(e, eb, sizeof(eb));
            SSL_free(s); pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error(std::string("db postgres: TLS handshake başarısız: ") + eb);
        }
        ssl_ = s;   // bundan sonra send/recv_bytes SSL üstünden (startup+auth+sorgu dahil)
    }
#else
    if (tls_)
        throw std::runtime_error("db postgres: TLS (postgresqls://) bu yapıda desteklenmiyor "
                                 "(Windows OpenSSL'siz derlenir). Linux yapısını kullanın.");
#endif

    send_startup();
    do_auth();
}

// ── Startup ───────────────────────────────────────────────────────────────────

void PostgresClient::send_startup() {
    // Startup: len(4) + protocol(4=0x00030000) + params + '\0'
    std::vector<uint8_t> body;
    uint8_t proto[4];
    write_u32_be(proto, 196608); // 3 << 16
    body.insert(body.end(), proto, proto + 4);

    auto append_param = [&](const char* key, const std::string& val) {
        while (*key) body.push_back((uint8_t)*key++);
        body.push_back('\0');
        body.insert(body.end(), val.begin(), val.end());
        body.push_back('\0');
    };
    append_param("user", user_);
    append_param("database", database_);
    append_param("client_encoding", "UTF8");
    // GÜVENLİK: string literal escape'i (bind_params → '' ile tek-tırnak) yalnız
    // standard_conforming_strings=on iken TAM güvenlidir; kapalıysa backslash bir
    // escape karakteri olur ve `\'` ile injection mümkün olur. Bağlantı düzeyinde
    // zorla (PG 9.1+ varsayılanı on ama enforce etmek MySQL'in NO_BACKSLASH_ESCAPES
    // temizlemesiyle aynı disiplin — server/rol config'inden bağımsız güvence).
    append_param("standard_conforming_strings", "on");
    body.push_back('\0'); // param list sonu

    // Startup mesajı: type byte YOK, sadece len + body
    uint32_t total = 4 + (uint32_t)body.size();
    uint8_t lb[4];
    write_u32_be(lb, total);
    send_bytes(lb, 4);
    send_bytes(body.data(), body.size());
}

// ── Auth ──────────────────────────────────────────────────────────────────────

// pg_parse_error → look/pg_parse.h (saf, fuzz ile paylasilir)

void PostgresClient::do_auth() {
    while (true) {
        auto msg = read_message();

        if (msg.type == 'R') {
            if (msg.body.size() < 4)
                throw std::runtime_error("db postgres: malformed auth message");
            uint32_t auth_type = read_u32_be(msg.body.data());

            if (auth_type == 0) {
                // AuthenticationOk — devam
            } else if (auth_type == 3) {
                // Cleartext
                std::vector<uint8_t> body(password_.begin(), password_.end());
                body.push_back('\0');
                send_message('p', body);
            } else if (auth_type == 5) {
                // MD5
                if (msg.body.size() < 8)
                    throw std::runtime_error("db postgres: missing MD5 salt");
                uint8_t salt[4];
                memcpy(salt, msg.body.data() + 4, 4);
                std::string pwd = pg_md5_password(password_, user_, salt);
                std::vector<uint8_t> body(pwd.begin(), pwd.end());
                body.push_back('\0');
                send_message('p', body);
            } else if (auth_type == 10) {
                // SASL (SCRAM-SHA-256 / SCRAM-SHA-256-PLUS)
                do_scram_sha256(msg.body);
            } else {
                throw std::runtime_error("db postgres: unsupported auth method " + std::to_string(auth_type)
                    + " — use scram-sha-256, md5, or trust in pg_hba.conf");
            }
        } else if (msg.type == 'E') {
            throw std::runtime_error("db postgres: " + pg_parse_error(msg.body));
        } else if (msg.type == 'Z') {
            // ReadyForQuery — auth tamamlandı
            return;
        }
        // 'S' (ParameterStatus), 'K' (BackendKeyData), 'N' (Notice) → yoksay
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

std::vector<DbRow> PostgresClient::query(const std::string& sql) {
    if (sock_ == PG_SOCK_INVALID) {
        try { do_connect(); } catch (...) {}
        if (sock_ == PG_SOCK_INVALID)
            throw std::runtime_error("db postgres: not connected");
    }

    try {
        return simple_query(sql);
    } catch (const PgSendError&) {
        // Yalnız GÖNDERİM başarısızsa retry — istek sunucuya ulaşmadı, güvenli.
        // Yanıt-okuma aşamasındaki kopma buraya DÜŞMEZ (std::runtime_error) →
        // propagate eder, retry edilmez (sunucu işlemiş olabilir → çift-yazma).
        try { do_connect(); } catch (...) { throw; }
        return simple_query(sql);
    }
}

std::vector<DbRow> PostgresClient::simple_query(const std::string& sql) {
    // Query mesajı: 'Q' + sql + '\0'
    std::vector<uint8_t> body(sql.begin(), sql.end());
    body.push_back('\0');
    send_message('Q', body);

    std::vector<DbRow> rows;
    struct ColInfo { std::string name; uint8_t type_code; };
    std::vector<ColInfo> columns;

    affected_rows_  = 0;
    last_insert_id_ = 0;

    // SQL INSERT mi? (lastval() için)
    bool is_insert = false;
    {
        const char* p = sql.c_str();
        while (*p && isspace((unsigned char)*p)) p++;
        char upper6[7] = {};
        for (int i = 0; i < 6 && p[i]; i++) upper6[i] = (char)toupper((unsigned char)p[i]);
        is_insert = (strcmp(upper6, "INSERT") == 0);
    }

    while (true) {
        PgMsg msg;
        try { msg = read_message(); }
        catch (...) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; throw; }

        if (msg.type == 'T') {
            // RowDescription
            columns.clear();
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            for (int i = 0; i < (int)ncols && p < end; i++) {
                // col name (null-term)
                std::string name;
                while (p < end && *p) name += (char)*p++;
                if (p < end) p++; // null
                // tableOID(4) + colAttr(2) = 6 bytes
                if (p + 6 > end) break;
                p += 6;
                // typeOID (4 bytes)
                if (p + 4 > end) break;
                int32_t type_oid = read_i32_be(p); p += 4;
                // typeSize(2) + typeMod(4) + format(2) = 8 bytes
                if (p + 8 <= end) p += 8;
                columns.push_back({std::move(name), oid_to_type(type_oid)});
            }
        } else if (msg.type == 'D') {
            // DataRow
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            DbRow row;
            // Bozuk DataRow SESSIZCE yanlis veri uretiyordu (sahte sunucuyla olculdu):
            //   uzunluk 100 der, 2 bayt gonderir  -> alan "" (bos string), hata YOK
            //   int32-max uzunluk                 -> alan "" , hata YOK
            //   3 alan vaat eder, 1 tane gonderir -> satir 1 sutunla doner, hata YOK
            // Uygulama bos degeri gercek veri sanip ona gore davraniyordu (bos yetki
            // dizesi, bos bakiye...). Cokme yoktu — sinir kontrolu tutuyordu — ama
            // sessiz yanlis veri bu projede cokmeden daha tehlikeli sayilir.
            // Ayrica 'p + field_len <= end' isaretci TASMASINA acikti (field_len
            // int32-max iken p+field_len tanimsiz davranis); 'field_len > end - p'
            // tasma-guvenli bicim.
            for (int i = 0; i < (int)ncols; i++) {
                if (end - p < 4)
                    throw std::runtime_error("db postgres: bozuk DataRow — " +
                        std::to_string(ncols) + " alan bildirildi, " + std::to_string(i) + " alan geldi");
                int32_t field_len = read_i32_be(p); p += 4;
                std::string val;
                bool is_null = false;
                if (field_len == -1) {
                    is_null = true;                       // protokolde NULL yalnizca -1
                } else if (field_len < 0) {
                    throw std::runtime_error("db postgres: bozuk DataRow — gecersiz alan uzunlugu " +
                                             std::to_string(field_len));
                } else if ((size_t)field_len > (size_t)(end - p)) {
                    throw std::runtime_error("db postgres: bozuk DataRow — alan uzunlugu " +
                        std::to_string(field_len) + " govdede kalan " +
                        std::to_string(end - p) + " bayti asiyor");
                } else {
                    val.assign((const char*)p, (size_t)field_len);
                    p += field_len;
                }
                uint8_t tc = (i < (int)columns.size()) ? columns[i].type_code : 0xFE;
                std::string name = (i < (int)columns.size()) ? columns[i].name : "col" + std::to_string(i);
                if (is_null) tc = pg_type::NUL;
                row.push_back({name, DbValue{val, tc, is_null}});
            }
            rows.push_back(std::move(row));
        } else if (msg.type == 'C') {
            // CommandComplete: "INSERT 0 5", "SELECT 3", "UPDATE 2" ...
            std::string tag((const char*)msg.body.data(),
                            msg.body.empty() ? 0 : msg.body.size() - 1);
            auto sp = tag.rfind(' ');
            if (sp != std::string::npos) {
                try { affected_rows_ = std::stoll(tag.substr(sp + 1)); } catch (...) {}
            }
        } else if (msg.type == 'Z') {
            // ReadyForQuery — bitti. Body[0] = transaction durum baytı.
            if (!msg.body.empty()) tx_status_ = (char)msg.body[0];
            break;
        } else if (msg.type == 'E') {
            std::string err = "db postgres: " + pg_parse_error(msg.body);
            // ReadyForQuery'ye kadar drain et
            while (true) {
                try { auto m = read_message(); if (m.type == 'Z') break; } catch (...) { break; }
            }
            throw std::runtime_error(err);
        }
        // 'N' (Notice), 'I' (EmptyQuery) → yoksay
    }

    // INSERT sonrası lastval() ile son ID'yi al.
    //
    // ESKİ HATA — sessiz VERİ KAYBI (gerçek PG sunucusuyla + sunucu loguyla bulundu):
    // Tablo SERIAL/sequence KULLANMIYORSA `SELECT lastval()` hata verir
    // ("lastval is not yet defined in this session"). Autocommit'te zararsız —
    // INSERT ayrı statement'ta zaten kalıcı. AMA açık transaction içinde bu hata
    // TÜM transaction'ı ABORTED yapar; sonraki COMMIT sessizce ROLLBACK'e döner:
    //   BEGIN; INSERT INTO t(n) VALUES(8);  -- affected=1
    //   SELECT lastval();                   -- HATA → transaction aborted
    //   COMMIT;                             -- aborted → ROLLBACK → INSERT KAYBOLUR
    // C++ tarafında catch(...) hatayı yutuyordu ama PG bağlantısı zaten zehirli.
    // sequence'siz tabloya transaction içinde INSERT yapan HERKES verisini
    // kaybediyordu (MySQL/SQLite'ta yok — lastval hataları oraya özgü değil).
    //
    // Çözüm: transaction bloğundaysak (tx_status_ == 'T') lastval'i SAVEPOINT ile
    // koru — hata olsa da transaction kurtulur. Autocommit'te ('I') doğrudan çağır.
    if (is_insert && affected_rows_ > 0) {
        // simple_query her çağrı başında affected_rows_/last_insert_id_'yi SIFIRLAR;
        // aşağıdaki SAVEPOINT/ROLLBACK/RELEASE çağrıları da simple_query olduğu için
        // INSERT'in değerlerini ezerdi → yereli tut, en sonda yaz.
        const int64_t saved_affected = affected_rows_;
        const bool in_tx = (tx_status_ == 'T');
        if (in_tx) { try { simple_query("SAVEPOINT look_lv"); } catch (...) {} }
        int64_t new_id = 0;
        try {
            auto lv = do_lastval();
            if (!lv.empty() && !lv[0].empty())
                new_id = std::stoll(lv[0][0].second.str);
        } catch (...) {}
        if (in_tx) {
            // lastval hata verdiyse transaction aborted — savepoint'e dönerek kurtar.
            // Başarılıysa da geri dönmek zararsız (lastval yalnızca okuma).
            try { simple_query("ROLLBACK TO SAVEPOINT look_lv"); } catch (...) {}
            try { simple_query("RELEASE SAVEPOINT look_lv"); } catch (...) {}
        }
        last_insert_id_ = new_id;
        affected_rows_  = saved_affected;  // savepoint komutları ezmesin
    }

    return rows;
}

// Lastval() için internal sorgu — reconnect döngüsü yok
std::vector<DbRow> PostgresClient::do_lastval() {
    std::string sql = "SELECT lastval()";
    std::vector<uint8_t> body(sql.begin(), sql.end());
    body.push_back('\0');
    send_message('Q', body);

    std::vector<DbRow> rows;
    while (true) {
        PgMsg msg;
        try { msg = read_message(); } catch (...) { break; }
        if (msg.type == 'D' && msg.body.size() >= 6) {
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            if (p + 4 <= end) {
                int32_t flen = read_i32_be(p); p += 4;
                if (flen >= 0 && p + flen <= end) {
                    DbRow row;
                    row.push_back({"lastval", DbValue{std::string((const char*)p, flen), pg_type::INT}});
                    rows.push_back(row);
                }
            }
        } else if (msg.type == 'Z') break;
    }
    return rows;
}

// ── Extended Query Protocol (Parse/Bind/Execute/Sync) ────────────────────────

std::vector<DbRow> PostgresClient::extended_query(const std::string& sql,
                                                    const std::vector<DbParam>& params) {
    // ? → $1, $2, ... dönüşümü
    std::string pg_sql;
    int param_idx = 0;
    bool in_str = false; char str_ch = 0;
    for (size_t i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (!in_str && (c == '\'' || c == '"')) { in_str = true; str_ch = c; pg_sql += c; continue; }
        if (in_str && c == str_ch) {
            // SQL kaçışı: '' (veya "") = literal tırnak — string KAPANMAZ. Aksi halde
            // 'it''s ? here' içindeki ? yanlışlıkla parametre sanılıp bind kayardı.
            if (i + 1 < sql.size() && sql[i + 1] == str_ch) { pg_sql += c; pg_sql += sql[++i]; continue; }
            in_str = false; pg_sql += c; continue;
        }
        if (!in_str && c == '?') { pg_sql += '$'; pg_sql += std::to_string(++param_idx); }
        else pg_sql += c;
    }

    int n = (int)params.size();

    // Parse ('P') — anonymous statement
    {
        std::vector<uint8_t> body;
        body.push_back('\0'); // statement name: ""
        body.insert(body.end(), pg_sql.begin(), pg_sql.end());
        body.push_back('\0'); // query
        body.push_back(0); body.push_back(0); // num_param_types = 0 (server infers)
        send_message('P', body);
    }

    // Bind ('B') — bind params as text format
    {
        std::vector<uint8_t> body;
        body.push_back('\0'); // portal name
        body.push_back('\0'); // statement name
        body.push_back(0); body.push_back(0); // num_format_codes = 0 (all text)
        // num_params
        body.push_back((n>>8)&0xFF); body.push_back(n&0xFF);
        for (int i = 0; i < n; i++) {
            if (params[i].kind == DbParam::NULL_VAL) {
                // -1 = NULL
                body.push_back(0xFF); body.push_back(0xFF);
                body.push_back(0xFF); body.push_back(0xFF);
            } else {
                std::string vs;
                switch (params[i].kind) {
                    case DbParam::INT_VAL:   vs = std::to_string(params[i].i); break;
                    case DbParam::FLOAT_VAL: vs = look_format_double(params[i].d); break;  // dilin tek double formatı
                    case DbParam::BOOL_VAL:  vs = params[i].b ? "true" : "false"; break;
                    default:                 vs = params[i].s; break;
                }
                int32_t vlen = (int32_t)vs.size();
                body.push_back((vlen>>24)&0xFF); body.push_back((vlen>>16)&0xFF);
                body.push_back((vlen>>8)&0xFF);  body.push_back(vlen&0xFF);
                body.insert(body.end(), vs.begin(), vs.end());
            }
        }
        body.push_back(0); body.push_back(0); // num_result_format_codes = 0 (text)
        send_message('B', body);
    }

    // Describe ('D') — portal
    { std::vector<uint8_t> body; body.push_back('P'); body.push_back('\0'); send_message('D', body); }

    // Execute ('E')
    { std::vector<uint8_t> body; body.push_back('\0'); for(int b=0;b<4;b++) body.push_back(0); send_message('E', body); }

    // Sync ('S')
    { send_message('S', {}); }

    // Drain responses
    std::vector<DbRow> rows;
    struct ColInfo { std::string name; uint8_t type_code; };
    std::vector<ColInfo> columns;
    affected_rows_ = 0;
    last_insert_id_ = 0;
    bool is_insert = false;
    {
        const char* p = sql.c_str();
        while (*p && isspace((unsigned char)*p)) p++;
        char u[7]={};
        for(int i=0;i<6&&p[i];i++) u[i]=(char)toupper((unsigned char)p[i]);
        is_insert = (strcmp(u,"INSERT")==0);
    }

    while (true) {
        PgMsg msg;
        try { msg = read_message(); } catch (...) { pg_close_sock(sock_); sock_=PG_SOCK_INVALID; throw; }

        if (msg.type == '1' || msg.type == '2') {
            // ParseComplete / BindComplete — devam
        } else if (msg.type == 'T') {
            columns.clear();
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            for (int i = 0; i < (int)ncols && p < end; i++) {
                std::string name;
                while (p < end && *p) name += (char)*p++;
                if (p < end) p++;
                if (p + 6 > end) break; p += 6;
                if (p + 4 > end) break;
                int32_t oid = read_i32_be(p); p += 4;
                if (p + 8 <= end) p += 8;
                columns.push_back({std::move(name), oid_to_type(oid)});
            }
        } else if (msg.type == 'D') {
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            DbRow row;
            for (int i = 0; i < (int)ncols && p + 4 <= end; i++) {
                int32_t flen = read_i32_be(p); p += 4;
                bool is_null = (flen < 0);
                std::string val;
                if (!is_null && p + flen <= end) { val = std::string((const char*)p, flen); p += flen; }
                uint8_t tc  = (i < (int)columns.size()) ? columns[i].type_code : 0xFE;
                std::string name = (i < (int)columns.size()) ? columns[i].name : "col" + std::to_string(i);
                if (is_null) tc = pg_type::NUL;
                row.push_back({name, DbValue{val, tc, is_null}});
            }
            rows.push_back(std::move(row));
        } else if (msg.type == 'C') {
            std::string tag((const char*)msg.body.data(), msg.body.empty()?0:msg.body.size()-1);
            auto sp = tag.rfind(' ');
            if (sp != std::string::npos) {
                try { affected_rows_ = std::stoll(tag.substr(sp+1)); } catch (...) {}
            }
        } else if (msg.type == 'Z') {
            break;
        } else if (msg.type == 'E') {
            std::string err = "db postgres: " + pg_parse_error(msg.body);
            while (true) { try { auto m=read_message(); if(m.type=='Z') break; } catch(...){break;} }
            throw std::runtime_error(err);
        }
    }

    if (is_insert && affected_rows_ > 0) {
        try {
            auto lv = do_lastval();
            if (!lv.empty() && !lv[0].empty())
                last_insert_id_ = std::stoll(lv[0][0].second.str);
        } catch (...) { last_insert_id_ = 0; }
    }
    return rows;
}

std::vector<DbRow> PostgresClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    if (sock_ == PG_SOCK_INVALID) {
        try { do_connect(); } catch (...) {}
        if (sock_ == PG_SOCK_INVALID)
            throw std::runtime_error("db postgres: not connected");
    }
    try {
        return extended_query(sql, params);
    } catch (const PgSendError&) {
        // Yalnız gönderim başarısızsa retry (bkz. query()/53. bug). Yanıt-okuma
        // kopması retry EDİLMEZ — sunucu statement'ı işlemiş olabilir.
        try { do_connect(); } catch (...) { throw; }
        return extended_query(sql, params);
    }
}

} // namespace look

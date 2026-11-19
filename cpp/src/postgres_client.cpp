#include "look/postgres_client.h"
#include "look/sock_idle.h"   // idle_socket_unusable: liveness probe before sending
#include "look/pg_parse.h"   // pure pg_parse_error (the single definition shared with fuzz/tests)
#include "look/pg_insert.h"  // INSERT identity: target table, RETURNING (pure, table-tested)
#include "look/format.h"     // look_format_double (the language's single double format)
#include "look/crypto_sha256.h" // SCRAM primitives: look::sha256/hmac_sha256/pbkdf2_sha256 (single impl)
#include "look/fiber_io.h"      // waiting inside a fiber: only the fiber waits, not the thread
#include "look/fiber_net.h"     // DNS/connect/TLS/send must not hold the thread inside a fiber
#include <cerrno>
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
  #include <poll.h>
  #include <fstream>
  // DB-TLS: PostgreSQL SSLRequest → TLS upgrade (Linux; Windows builds without OpenSSL).
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#include "look/tls_peer.h"   // SNI + name verification (IP → IP SAN), handshake failure reason
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0   // Windows/macOS: no such flag (Windows has no SIGPIPE either)
#endif

namespace look {

// The SCRAM-SHA-256 client nonce must be CRYPTOGRAPHIC (RFC 5802 §5.1: the nonce
// must be unpredictable). std::mt19937 is predictable (32-bit random_device
// seed) → not a CSPRNG. Use a fail-closed secure source.
static void pg_secure_random(uint8_t* buf, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("PostgreSQL SCRAM: CSPRNG unavailable (BCryptGenRandom)");
#else
    std::ifstream u("/dev/urandom", std::ios::binary);
    u.read(reinterpret_cast<char*>(buf), (std::streamsize)n);
    if (!u || (size_t)u.gcount() != n)
        throw std::runtime_error("PostgreSQL SCRAM: CSPRNG unavailable (/dev/urandom)");
#endif
}

#ifndef _WIN32
// ── DB-TLS: client SSL_CTX ───────────────────────────────────────────────────
// TLS is NEW in PostgreSQL (no users to break) → per-SSL verify is enabled with the CORRECT default
// (SSL_VERIFY_PEER + SSL_set1_host). The CTX only loads the trust store; the verify mode
// is set per connection (the mysql_client:601 pattern, NOT the VERIFY_NONE of :55).
static SSL_CTX* pg_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        look::tls_client_defaults(c);   // at least TLS 1.2, independent of the machine
        if (c) SSL_CTX_set_default_verify_paths(c);   // system CAs (for the verify mode)
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
// MERGED: the SCRAM primitives now delegate to the SINGLE impl in look::crypto_sha256.h.
// A PARALLEL copy of sha256/hmac/pbkdf2 used to live here ("same rule in two places"); it was removed after the RFC KAT
// (tests/pg_scram_crypto_kat.cpp) proved the two copies DO NOT DIVERGE →
// single definition → no drift. These three methods are only an array<32>↔vector adapter; the merge logic of do_scram_sha256
// (the live auth path) did NOT CHANGE (it still takes array<32>). Details: crypto_sha256.h.

static inline std::array<uint8_t,32> to_arr32(const std::vector<uint8_t>& v) {
    std::array<uint8_t,32> out{};
    for (int i = 0; i < 32 && i < (int)v.size(); i++) out[i] = v[i];
    return out;
}

std::array<uint8_t,32> PostgresClient::sha256(const uint8_t* data, size_t len) {
    return to_arr32(look::sha256(std::vector<uint8_t>(data, data + len)));
}

std::array<uint8_t,32> PostgresClient::hmac_sha256(const uint8_t* key, size_t klen,
                                                     const uint8_t* msg, size_t mlen) {
    return to_arr32(look::hmac_sha256(std::vector<uint8_t>(key, key + klen),
                                      std::vector<uint8_t>(msg, msg + mlen)));
}

std::array<uint8_t,32> PostgresClient::pbkdf2_sha256(const std::string& pass,
                                                       const uint8_t* salt, size_t slen, int iters) {
    // PRF = HMAC-SHA-256; PG SCRAM is single-block (dkLen=32). look::pbkdf2 also knows multi-block but
    // 32 is requested here → the single-block path (INT(1) big-endian) exactly.
    return to_arr32(look::pbkdf2_sha256(pass, std::vector<uint8_t>(salt, salt + slen),
                                        (uint32_t)iters, 32));
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
    // 18-byte CSPRNG nonce (RFC 5802: must be unpredictable — not mt19937)
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
                // the SCRAM iteration count comes from the server — a bad value → 0, rejected below as iter<=0
                try { iter = std::stoi(tok.substr(2)); } catch (...) { iter = 0; }
            }
        }
    }
    if (s_nonce.empty() || s_salt_b64.empty() || iter <= 0)
        throw std::runtime_error("db postgres: malformed SASLContinue");
    // The server supplies the iteration count: a malicious (or intercepting) server could keep the thread opening
    // the connection in PBKDF2 for minutes with i=2147483647. The PG default is 4096;
    // OWASP recommends 600 000 — anything above 1 000 000 is rejected (with a reason).
    if (iter > 1000000)
        throw std::runtime_error("db postgres: the server asks for " + std::to_string(iter) +
                                 " SCRAM iterations (limit 1000000) — refusing to connect");
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
    // INTEGER types
    if (oid == 20 || oid == 21 || oid == 23 || oid == 26 || oid == 16000)
        return pg_type::INT;
    // FLOAT / NUMERIC types
    if (oid == 700 || oid == 701 || oid == 1700)
        return pg_type::FLOAT;
    // BOOL
    if (oid == 16)
        return pg_type::BOOL;
    // NULL and unknown → string
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
        // send the Terminate message (best effort) — if TLS is active send_bytes goes through SSL
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
    if (ssl_) {   // TLS active → SSL_read
        SSL* s = (SSL*)ssl_;
        while (received < len) {
            // Inside a fiber: if the TLS buffer is empty only the fiber waits until the socket is readable.
            if (SSL_pending(s) == 0) {
                int w = fiber_wait_readable(sock_, rcv_tmo_ms_);
                if (w == 0) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; return false; }
            }
            int r = SSL_read(s, buf + received, (int)(len - received));
            if (r <= 0) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; return false; }
            received += r;
        }
        return true;
    }
#endif
    // ── Plain TCP: buffered read (same path as MySQL) ────────────────────────
    // One recv fills the buffer; messages (type + length + body) are served from the buffer. It used to be three recvs per
    // message: a single-row SELECT took ~18 system calls. A request larger than the buffer is read
    // directly into the target when the buffer is empty.
    if (!rbuf_) rbuf_.reset(new uint8_t[RBUF_SIZE]);
    while (received < len) {
        size_t avail = rbuf_len_ - rbuf_pos_;
        if (avail > 0) {
            size_t n = std::min(avail, len - received);
            std::memcpy(buf + received, rbuf_.get() + rbuf_pos_, n);
            rbuf_pos_ += n; received += n;
            continue;
        }
        rbuf_pos_ = rbuf_len_ = 0;
        if (len - received >= RBUF_SIZE) {
            int r = recv_some(buf + received, len - received);
            if (r <= 0) return false;
            received += (size_t)r;
            continue;
        }
        int r = recv_some(rbuf_.get(), RBUF_SIZE);
        if (r <= 0) return false;
        rbuf_len_ = (size_t)r;
    }
    return true;
}

// At least 1, at most cap bytes from the plain TCP socket. <=0: connection ended (socket closed).
int PostgresClient::recv_some(uint8_t* buf, size_t cap) {
#ifndef _WIN32
    // Fiber path: read without waiting; if there is no data suspend the fiber (same path as MySQL).
    if (Fiber::current()) {
        for (;;) {
            ssize_t r = ::recv(sock_, (char*)buf, cap, MSG_DONTWAIT);
            if (r > 0) return (int)r;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int w = fiber_wait_readable(sock_, rcv_tmo_ms_);
                if (w == 1) continue;
                if (w < 0) {   // no timer → blocking read
                    r = ::recv(sock_, (char*)buf, cap, 0);
                    if (r > 0) return (int)r;
                }
            }
            pg_close_sock(sock_);
            sock_ = PG_SOCK_INVALID;
            return -1;
        }
    }
#endif
    // Non-fiber path (Windows, command line) — blocking-ok: no fiber
    int r = recv(sock_, (char*)buf, (int)cap, 0);
    if (r <= 0) {
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
        return -1;
    }
    return r;
}

// A dedicated type that separates a drop BEFORE THE REQUEST REACHED THE SERVER ("at-most-once safe") from a drop
// during response reading. query()/execute() retry ONLY on this type —
// a request that could not be sent was NOT processed by the server, retrying is safe.
// A drop while reading the response is NOT this type → it is not retried (the server may have
// processed the INSERT; a blind retry produces a silent double write — proven with the fake server
// "commit_kopar").
struct PgSendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void PostgresClient::send_bytes(const uint8_t* buf, size_t len) {
#ifndef _WIN32
    // If the send buffer is full only the fiber waits inside a fiber (outside a fiber: SO_SNDTIMEO).
    // The error path is symmetric with the read paths: the socket is closed, sock_ INVALID (note below).
    if (!look::fiber_send_all(sock_, (SSL*)ssl_, buf, len, rcv_tmo_ms_)) {
        bool tls = ssl_ != nullptr;
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw PgSendError(tls ? "db postgres: TLS send failed — connection lost"
                              : "db postgres: send failed — connection lost");
    }
#else
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), MSG_NOSIGNAL);   // no SIGPIPE: EPIPE on a closed socket
        if (r <= 0) {
            // SYMMETRIC with the read paths: when the send drops close the socket and make sock_
            // INVALID so that an fd that is "closed but looks valid" does not go back to the pool and
            // poison the next query. (The `if sock_==INVALID → do_connect()` at the start of
            // query()/execute() catches this automatically.) A source audit
            // found this asymmetry; no observable bug could be
            // produced — the PgSendError retry already recovers the recoverable cases —
            // but the symmetry is defense in depth.
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw PgSendError("db postgres: send failed — connection lost");
        }
        sent += r;
    }
#endif
}

void PostgresClient::set_socket_timeout(int ms) {
    if (sock_ == PG_SOCK_INVALID) return;
    rcv_tmo_ms_ = ms;   // the fiber wait is bounded by the same duration (SO_RCVTIMEO does not work in a fiber)
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

// Backend → type(1) + len(4, len includes itself but not the type) + body
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
    // A server-controlled len can be up to ~4GB → std::vector(body_len) is an unbounded
    // allocation → OOM/crash (malicious/MITM server, no TLS). A reasonable upper limit
    // (default 256MB, set with LOOK_PG_MAX_MSG).
    static const size_t PG_MAX_MSG = []() -> size_t {
        const char* e = std::getenv("LOOK_PG_MAX_MSG");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return 256ull * 1024 * 1024;
    }();
    if (body_len > PG_MAX_MSG)
        throw std::runtime_error("db postgres: message size limit exceeded (LOOK_PG_MAX_MSG)");
    std::vector<uint8_t> body(body_len);
    if (body_len > 0 && !recv_bytes(body.data(), body_len))
        throw std::runtime_error("db postgres: connection lost reading body");
    return PgMsg{(char)type_byte, std::move(body)};
}

// Frontend → type(1) + len(4, len includes itself) + body
void PostgresClient::send_message(char type, const std::vector<uint8_t>& body) {
    queue_message(type, body);
    flush_out();
}

// Append the message to the send queue (flush_out sends it with a single send).
void PostgresClient::queue_message(char type, const std::vector<uint8_t>& body) {
    uint32_t len = 4 + (uint32_t)body.size();
    uint8_t hdr[5];
    hdr[0] = (uint8_t)type;
    write_u32_be(hdr + 1, len);
    out_.insert(out_.end(), hdr, hdr + 5);
    out_.insert(out_.end(), body.begin(), body.end());
}

void PostgresClient::flush_out() {
    if (out_.empty()) return;
    std::vector<uint8_t> o;
    o.swap(out_);   // even if the send throws the queue stays empty (a half message does not leak into the next query)
    send_bytes(o.data(), o.size());
    if (o.capacity() <= 65536) { o.clear(); out_.swap(o); }   // reuse a small buffer (drop a large one)
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
#ifndef _WIN32
    // The old TLS session was not released: on reconnect the SSLRequest went encrypted over the
    // old session, the server did not understand → a TLS connection could never be established again (+ a leak).
    if (ssl_) { SSL_free((SSL*)ssl_); ssl_ = nullptr; }
#endif
    if (sock_ != PG_SOCK_INVALID) {
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
    }
    // New session: unread bytes and unsent messages left from the old connection belonged to that session —
    // if read on the new connection the replies would shift (rows of another query would arrive).
    rbuf_pos_ = rbuf_len_ = 0;
    out_.clear();

    // Go net.Dial model (IPv4 + IPv6, try addresses in order) — look/fiber_net.h tcp_dial.
    // While connecting and resolving names inside a fiber only the fiber waits.
    {
        bool resolved = false, timed_out = false;
        auto s = tcp_dial(host_, port_, connect_timeout_ms_, resolved, timed_out);
        if (s == DIAL_INVALID) {
            if (!resolved) throw std::runtime_error("db postgres: cannot resolve host: " + host_);
            throw std::runtime_error((timed_out ? "db postgres: timeout connecting to " : "db postgres: cannot connect to ") +
                                     join_host_port(host_, port_));
        }
        sock_ = s;
    }

    // Turn off Nagle's algorithm — avoids a 40ms delay on small packets
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
        // SSLRequest (len=8, code=80877103=0x04D2162F) → the server answers 1 byte: 'S' accept / 'N' no.
        // On 'N' DO NOT SILENTLY FALL BACK TO PLAINTEXT — worse than never adding TLS (the user
        // thinks it is encrypted). If tls_ was requested, give a clean error.
        uint8_t sslreq[8] = {0,0,0,8, 0x04,0xD2,0x16,0x2F};
        send_bytes(sslreq, 8);   // ssl_ does not exist yet → plaintext
        uint8_t resp = 0;
        if (!recv_bytes(&resp, 1)) {
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error("db postgres: could not read SSLRequest response");
        }
        // If bytes remain in the buffer after 'S', reject: the server (or an interceptor) sent plaintext
        // before TLS; those bytes would be read as if they were the first reply of the encrypted session
        // (the CVE-2021-23222 class in PostgreSQL). An honest server waits for TLS after 'S'.
        if (resp == 'S' && rbuf_len_ != rbuf_pos_) {
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error("db postgres: the server sent data before TLS started — connection refused");
        }
        if (resp != 'S') {
            pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error(std::string("db postgres: server does not support TLS "
                "(SSLRequest response '") + (char)resp + "') — did NOT fall back to plaintext. "
                "If you want to connect without encryption, use postgres://.");
        }
        SSL_CTX* ctx = pg_ssl_ctx();
        SSL* s = ctx ? SSL_new(ctx) : nullptr;
        if (!s) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
                  throw std::runtime_error("db postgres: SSL_new failed (TLS)"); }
        SSL_set_fd(s, (int)sock_);
        // SNI + name verification (IP → IP SAN, name → DNS name; look/tls_peer.h). If it cannot be set up, name
        // checking would silently turn off → "valid CA + wrong host" MITM opens → reject.
        if (tls_verify_) SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
        if (!tls_set_peer(s, host_, tls_verify_)) {
            SSL_free(s); pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error("db postgres: could not set up TLS hostname verification");
        }
        int hs = fiber_ssl_connect(s, (int)sock_, connect_timeout_ms_);
        if (hs != 1) {
            std::string why = hs == 0 ? std::string() : tls_failure_reason(s);   // look/tls_peer.h
            SSL_free(s); pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
            throw std::runtime_error(hs == 0 ? std::string("db postgres: TLS handshake timed out")
                                             : "db postgres: TLS handshake failed: " + why);
        }
        ssl_ = s;   // from now on send/recv_bytes go over SSL (including startup+auth+query)
    }
#else
    if (tls_)
        throw std::runtime_error("db postgres: TLS (postgresqls://) is not supported in this build "
                                 "(Windows is built without OpenSSL). Use the Linux build.");
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
    // SECURITY: string literal escaping (bind_params → single quote with '') is FULLY
    // safe only when standard_conforming_strings=on; if off, backslash becomes an
    // escape character and injection is possible with `\'`. Enforce at the connection level
    // (the PG 9.1+ default is on, but enforcing it is the same discipline as MySQL's NO_BACKSLASH_ESCAPES
    // clearing — a guarantee independent of server/role config).
    append_param("standard_conforming_strings", "on");
    body.push_back('\0'); // end of param list

    // Startup message: NO type byte, only len + body
    uint32_t total = 4 + (uint32_t)body.size();
    uint8_t lb[4];
    write_u32_be(lb, total);
    send_bytes(lb, 4);
    send_bytes(body.data(), body.size());
}

// ── Auth ──────────────────────────────────────────────────────────────────────

// pg_parse_error → look/pg_parse.h (pure, shared with fuzz)

void PostgresClient::do_auth() {
    while (true) {
        auto msg = read_message();

        if (msg.type == 'R') {
            if (msg.body.size() < 4)
                throw std::runtime_error("db postgres: malformed auth message");
            uint32_t auth_type = read_u32_be(msg.body.data());

            if (auth_type == 0) {
                // AuthenticationOk — continue
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
            // ReadyForQuery — auth complete (fresh connection: no transaction)
            in_txn_ = false;
            return;
        }
        // 'S' (ParameterStatus), 'K' (BackendKeyData), 'N' (Notice) → ignore
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

// BEFORE the query. If the server closed the idle connection (pg_terminate_backend, restart,
// idle timeout) detect it WITHOUT SENDING anything and renew it. A connection that drops inside a transaction is NOT
// replaced by a new one: the server rolled the transaction back; previously the following commands on the new connection
// were written permanently OUTSIDE the transaction (measured: silent inconsistency).
static const char* const kPgTxLost = "db postgres: the connection was closed inside a transaction; "
                                     "the server rolled the transaction back and nothing of it was kept";
bool PostgresClient::unread_reply_bytes() const {
    // Before a query is sent no unread reply bytes may remain: if they do, an earlier reply
    // was abandoned halfway and everything read on this connection is one query behind. A byte on the
    // socket is seen by idle_socket_unusable; this sees what is in the read buffer (and the TLS buffer).
    if (rbuf_len_ != rbuf_pos_) return true;
#ifndef _WIN32
    if (ssl_ && SSL_pending((SSL*)ssl_) > 0) return true;
#endif
    return false;
}

void PostgresClient::ready_to_send() {
    if (sock_ != PG_SOCK_INVALID && (unread_reply_bytes() || idle_socket_unusable(sock_))) disconnect();
    if (sock_ != PG_SOCK_INVALID) return;
    if (in_txn_) { in_txn_ = false; throw std::runtime_error(kPgTxLost); }
    try { do_connect(); } catch (...) {}
    if (sock_ == PG_SOCK_INVALID)
        throw std::runtime_error("db postgres: not connected");
}

std::vector<DbRow> PostgresClient::query(const std::string& sql) {
    ready_to_send();
    try {
        return simple_query(sql);
    } catch (const PgSendError&) {
        // Retry only if the SEND failed — the request did not reach the server, safe.
        // A drop during response reading does NOT land here (std::runtime_error) →
        // it propagates, no retry (the server may have processed it → double write).
        // No retry inside a transaction (the new connection would be outside the transaction).
        if (in_txn_) { in_txn_ = false; throw std::runtime_error(kPgTxLost); }
        try { do_connect(); } catch (...) { throw; }
        return simple_query(sql);
    }
}

// COPY ... FROM STDIN / TO STDOUT: db:: does not carry a data stream. Previously on FROM STDIN the server waited for
// data from the client and the client for a reply from the server → it hung until the read timeout (30 s) with an unrelated
// error; TO STDOUT silently discarded the rows and returned an empty result. Now both fail immediately, with a reasoned error.
static std::vector<uint8_t> pg_copy_fail_body() {
    static const char m[] = "COPY FROM STDIN is not supported by db:: (send the rows with INSERT)";
    return std::vector<uint8_t>(m, m + sizeof(m));   // including NUL
}
static std::string pg_copy_out_unsupported() {
    return "db postgres: COPY TO STDOUT is not supported by db:: (read the rows with SELECT)";
}

std::vector<DbRow> PostgresClient::simple_query(const std::string& sql) {
    // Query message: 'Q' + sql + '\0'
    std::vector<uint8_t> body(sql.begin(), sql.end());
    body.push_back('\0');
    send_message('Q', body);

    std::vector<DbRow> rows;
    struct ColInfo { std::string name; uint8_t type_code; };
    std::vector<ColInfo> columns;

    affected_rows_  = 0;
    // last_insert_id_ is NOT TOUCHED: this path is only for BEGIN/COMMIT/ROLLBACK/SAVEPOINT (user
    // SQL goes through extended_query); after COMMIT db::last_id still gives that of the INSERT.

    // If an error is thrown before the reply is read to its end (ReadyForQuery) the connection is closed: unread
    // messages would be taken for the reply to the next query (every result one query behind; no error —
    // tests/pg_malformed_desync_test.sh). A server error ('E') is read up to 'Z', the connection stays.
    bool reply_done = false;
    bool copy_out = false;
    try {
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
                // col name (null-terminated)
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
            // A malformed DataRow SILENTLY produced wrong data (measured with a fake server):
            //   declares length 100, sends 2 bytes  -> field "" (empty string), NO error
            //   int32-max length                    -> field "" , NO error
            //   promises 3 fields, sends 1          -> row returns with 1 column, NO error
            // The application took the empty value for real data and behaved accordingly (empty authorization
            // string, empty balance...). There was no crash — the bounds check held — but
            // silently wrong data is considered more dangerous than a crash in this project.
            // Also 'p + field_len <= end' was open to pointer OVERFLOW (undefined behavior when field_len
            // is int32-max); 'field_len > end - p' is the
            // overflow-safe form.
            for (int i = 0; i < (int)ncols; i++) {
                if (end - p < 4)
                    throw std::runtime_error("db postgres: malformed DataRow — " +
                        std::to_string(ncols) + " fields declared, " + std::to_string(i) + " fields received");
                int32_t field_len = read_i32_be(p); p += 4;
                std::string val;
                bool is_null = false;
                if (field_len == -1) {
                    is_null = true;                       // in the protocol NULL is only -1
                } else if (field_len < 0) {
                    throw std::runtime_error("db postgres: malformed DataRow — invalid field length " +
                                             std::to_string(field_len));
                } else if ((size_t)field_len > (size_t)(end - p)) {
                    throw std::runtime_error("db postgres: malformed DataRow — field length " +
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
            // ReadyForQuery — done.
            in_txn_ = !msg.body.empty() && msg.body[0] != 'I';
            reply_done = true;
            if (copy_out) throw std::runtime_error(pg_copy_out_unsupported());
            break;
        } else if (msg.type == 'G' || msg.type == 'W') {
            send_message('f', pg_copy_fail_body());   // CopyFail → the server sends 'E' + 'Z'
        } else if (msg.type == 'H') {
            copy_out = true;                          // 'd'/'c' are swallowed, error at 'Z'
        } else if (msg.type == 'E') {
            std::string err = "db postgres: " + pg_parse_error(msg.body);
            // drain up to ReadyForQuery
            while (true) {
                try { auto m = read_message(); if (m.type == 'Z') { in_txn_ = !m.body.empty() && m.body[0] != 'I'; reply_done = true; break; } } catch (...) { break; }
            }
            throw std::runtime_error(err);
        }
        // 'N' (Notice), 'I' (EmptyQuery) → ignore
    }
    } catch (...) {
        if (!reply_done && sock_ != PG_SOCK_INVALID) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; }
        throw;
    }

    return rows;
}

// ── Extended Query Protocol (Parse/Bind/Execute/Sync) ────────────────────────

std::vector<DbRow> PostgresClient::extended_query_raw(const std::string& sql,
                                                    const std::vector<DbParam>& params) {
    // ? → $1, $2, ... conversion
    std::string pg_sql;
    int param_idx = 0;
    bool in_str = false; char str_ch = 0;
    for (size_t i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (!in_str && (c == '\'' || c == '"')) { in_str = true; str_ch = c; pg_sql += c; continue; }
        if (in_str && c == str_ch) {
            // SQL escape: '' (or "") = literal quote — the string does NOT close. Otherwise
            // the ? inside 'it''s ? here' would be mistaken for a parameter and the bind would shift.
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
        queue_message('P', body);   // Parse … Sync: a single send (flush at Sync)
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
                    case DbParam::FLOAT_VAL: vs = look_format_double(params[i].d); break;  // the language's single double format
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
        queue_message('B', body);
    }

    // Describe ('D') — portal
    { std::vector<uint8_t> body; body.push_back('P'); body.push_back('\0'); queue_message('D', body); }

    // Execute ('E')
    { std::vector<uint8_t> body; body.push_back('\0'); for(int b=0;b<4;b++) body.push_back(0); queue_message('E', body); }

    // Sync ('S') — the five queued messages go out with a single send
    { send_message('S', {}); }

    // Drain responses
    std::vector<DbRow> rows;
    struct ColInfo { std::string name; uint8_t type_code; };
    std::vector<ColInfo> columns;
    affected_rows_ = 0;

    // same rule as simple_query: if an error is thrown before the reply is read up to 'Z' the connection is closed.
    bool reply_done = false;
    bool copy_out = false;
    try {
    while (true) {
        PgMsg msg;
        try { msg = read_message(); } catch (...) { pg_close_sock(sock_); sock_=PG_SOCK_INVALID; throw; }

        if (msg.type == '1' || msg.type == '2') {
            // ParseComplete / BindComplete — continue
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
            // DataRow — the SAME guards as simple_query (a malformed DataRow must not silently
            // produce wrong data). db::query/exec/one now use this extended path;
            // the guards were only in simple_query → added here too (the differential caught it).
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            DbRow row;
            for (int i = 0; i < (int)ncols; i++) {
                if (end - p < 4)
                    throw std::runtime_error("db postgres: malformed DataRow — " +
                        std::to_string(ncols) + " fields declared, " + std::to_string(i) + " fields received");
                int32_t field_len = read_i32_be(p); p += 4;
                std::string val;
                bool is_null = false;
                if (field_len == -1) {
                    is_null = true;                       // in the protocol NULL is only -1
                } else if (field_len < 0) {
                    throw std::runtime_error("db postgres: malformed DataRow — invalid field length " +
                                             std::to_string(field_len));
                } else if ((size_t)field_len > (size_t)(end - p)) {   // overflow-safe
                    throw std::runtime_error("db postgres: malformed DataRow — field length " +
                        std::to_string(field_len) + " govdede kalan " +
                        std::to_string(end - p) + " bayti asiyor");
                } else {
                    val.assign((const char*)p, (size_t)field_len);
                    p += field_len;
                }
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
            in_txn_ = !msg.body.empty() && msg.body[0] != 'I';
            reply_done = true;
            if (copy_out) throw std::runtime_error(pg_copy_out_unsupported());
            break;
        } else if (msg.type == 'G' || msg.type == 'W') {
            // In copy mode the server ignored the previous Sync: a new Sync is needed after CopyFail
            queue_message('f', pg_copy_fail_body());
            send_message('S', {});
        } else if (msg.type == 'H') {
            copy_out = true;
        } else if (msg.type == 'E') {
            std::string err = "db postgres: " + pg_parse_error(msg.body);
            while (true) { try { auto m=read_message(); if(m.type=='Z') { in_txn_ = !m.body.empty() && m.body[0] != 'I'; reply_done = true; break; } } catch(...){break;} }
            throw std::runtime_error(err);
        }
    }
    } catch (...) {
        if (!reply_done && sock_ != PG_SOCK_INVALID) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; }
        throw;
    }
    return rows;
}

// The user's SQL. If it is an INSERT the id is read from the inserted row (look/pg_insert.h — the silent data loss and wrong id
// bug of the old lastval path is described there). The extra query is only a catalog
// query: it cannot error, it cannot break the transaction.
std::vector<DbRow> PostgresClient::extended_query(const std::string& sql,
                                                    const std::vector<DbParam>& params) {
    if (!pg_is_insert(sql)) {
        auto rows = extended_query_raw(sql, params);
        last_insert_id_ = 0;
        return rows;
    }
    const std::string table = pg_insert_target(sql);
    const std::string col   = table.empty() ? std::string() : serial_column(table);
    if (col.empty()) {                       // table without a sequence (or unrecognized form) → no id
        auto rows = extended_query_raw(sql, params);
        last_insert_id_ = 0;
        return rows;
    }
    // The id of the last of the inserted rows (like SQLite last_insert_rowid).
    auto id_of = [&](const std::vector<DbRow>& rows) -> int64_t {
        if (rows.empty()) return 0;
        for (const auto& f : rows.back())
            if (f.first == col && !f.second.is_null) { try { return std::stoll(f.second.str); } catch (...) { return 0; } }
        return 0;
    };
    if (pg_has_returning(sql)) {             // the user's own RETURNING — do not touch the SQL
        auto rows = extended_query_raw(sql, params);
        last_insert_id_ = id_of(rows);
        return rows;
    }
    auto rows = extended_query_raw(pg_with_returning(sql, col), params);
    last_insert_id_ = id_of(rows);
    return {};                               // the rows of the added RETURNING do not go to the user
}

std::string PostgresClient::serial_column(const std::string& table) {
    // to_regclass: NULL if the table does not exist (not an error); since the name passed through pg_insert_target the syntax is
    // valid. pg_get_serial_sequence knows both SERIAL and IDENTITY columns.
    auto rows = extended_query_raw(
        "SELECT a.attname FROM pg_catalog.pg_attribute a "
        "WHERE a.attrelid = to_regclass(?) AND a.attnum > 0 AND NOT a.attisdropped "
        "AND pg_catalog.pg_get_serial_sequence(a.attrelid::regclass::text, a.attname) IS NOT NULL "
        "ORDER BY a.attnum LIMIT 1", {DbParam::from_text(table)});
    if (rows.empty() || rows[0].empty()) return "";
    return rows[0][0].second.str;
}

std::vector<DbRow> PostgresClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    ready_to_send();
    try {
        return extended_query(sql, params);
    } catch (const PgSendError&) {
        // Retry only if the send failed (see query()). A drop during response reading
        // is NOT retried — the server may have processed the statement.
        if (in_txn_) { in_txn_ = false; throw std::runtime_error(kPgTxLost); }
        try { do_connect(); } catch (...) { throw; }
        return extended_query(sql, params);
    }
}

} // namespace look

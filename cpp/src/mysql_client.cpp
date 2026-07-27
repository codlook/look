#include "look/mysql_client.h"
#include "look/fiber.h"
#ifndef _WIN32
// caching_sha2_password (MySQL 8+ varsayilan): SHA-256 + RSA-OAEP.
// OpenSSL zaten linkli (POSIX); Windows'ta yalniz ws2_32+bcrypt linkleniyor —
// oradaki durum do_handshake icinde acikca ele aliniyor.
#  include <openssl/sha.h>
#  include <openssl/evp.h>
#  include <openssl/pem.h>
#  include <openssl/rsa.h>
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#endif
#include <cstring>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
  // MSG_DONTWAIT POSIX'e özgü — Windows'ta yok. Fiber path'inde kullanılır (fiber
  // Windows'ta zaten kapalı). Derleme için 0; soket blocking modda kalır.
  #ifndef MSG_DONTWAIT
    #define MSG_DONTWAIT 0
  #endif
#else
  #include <netinet/tcp.h>
  #include <poll.h>
#endif

namespace look {

// Sunucu-verili sütun sayısı üst sınırı — kötü niyetli/MITM sunucunun devasa
// col_count'la kaynak tükettiği DoS'a karşı. Gerçek query'ler ≪65535 sütun.
static constexpr uint64_t MYSQL_MAX_COLUMNS = 65535;

#ifndef _WIN32
// ── DB-TLS: client SSL_CTX ───────────────────────────────────────────────────
// MySQL protokolünde SSL, initial handshake'ten SONRA / auth'tan ÖNCE kurulur
// (SSLRequest paketi → TLS upgrade → auth artık şifreli). Amaç kablo şifreleme
// (--ssl-mode=REQUIRED): DB sertifikaları çoğu kez self-signed olduğundan PEER
// doğrulaması YAPMIYORUZ (aksi halde tipik kurulumda bağlantı kırılır); pasif
// dinlemeye karşı korur. Sertifika doğrulama gelecekte ayrı bir mod olabilir.
static SSL_CTX* mysql_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c) SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
        return c;
    }();
    return ctx;
}
#endif

// ── Pure C++ SHA1 — platform bagimsiz ────────────────────────────────────────

std::vector<uint8_t> MySQLClient::sha1(const std::vector<uint8_t>& data) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};

    std::vector<uint8_t> msg(data);
    uint64_t bit_len = (uint64_t)data.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    auto rot = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; j++)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 80; j++) {
            uint32_t v = w[j-3]^w[j-8]^w[j-14]^w[j-16];
            w[j] = rot(v, 1);
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; j++) {
            uint32_t f, k;
            if      (j<20) { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (j<40) { f=b^c^d;          k=0x6ED9EBA1; }
            else if (j<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d;          k=0xCA62C1D6; }
            uint32_t t = rot(a,5)+f+e+k+w[j];
            e=d; d=c; c=rot(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    std::vector<uint8_t> r(20);
    for (int i = 0; i < 5; i++) {
        r[i*4]=(h[i]>>24)&0xFF; r[i*4+1]=(h[i]>>16)&0xFF;
        r[i*4+2]=(h[i]>>8)&0xFF; r[i*4+3]=h[i]&0xFF;
    }
    return r;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

MySQLClient::MySQLClient() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) == 0)
        wsock_init_ = true;
#endif
}

MySQLClient::~MySQLClient() {
    disconnect();
#ifdef _WIN32
    if (wsock_init_) WSACleanup();
#endif
}

void MySQLClient::disconnect() {
#ifndef _WIN32
    if (ssl_) {
        SSL* s = (SSL*)ssl_;
        SSL_shutdown(s);   // TLS close_notify (best-effort)
        SSL_free(s);       // socket'i kapatmaz — sock_ ayrıca kapatılır
        ssl_ = nullptr;
    }
#endif
    if (sock_ != SOCK_INVALID) {
        close_sock(sock_);
        sock_ = SOCK_INVALID;
    }
}

// ── Socket timeout ────────────────────────────────────────────────────────────

void MySQLClient::set_socket_timeout(int ms) {
    if (sock_ == SOCK_INVALID) return;
#ifdef _WIN32
    DWORD timeout = (DWORD)ms;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ── Connection ────────────────────────────────────────────────────────────────

void MySQLClient::connect(const std::string& host, int port,
                           const std::string& user, const std::string& password,
                           const std::string& database) {
    cfg_.host=host; cfg_.port=port; cfg_.user=user;
    cfg_.password=password; cfg_.database=database;
    do_connect();
}

void MySQLClient::connect(const DbConfig& cfg) {
    cfg_ = cfg;
    do_connect();
}

void MySQLClient::do_connect() {
    if (sock_ != SOCK_INVALID) disconnect();

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == SOCK_INVALID)
        throw std::runtime_error("db: socket creation failed");

    // Non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)cfg_.port);

    struct addrinfo* res = nullptr;
    struct addrinfo  hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg_.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        disconnect();
        throw std::runtime_error("db: cannot resolve host: " + cfg_.host);
    }
    addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    ::connect(sock_, (struct sockaddr*)&addr, sizeof(addr));

    // Wait for connect with timeout
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock_, &wfds);
    struct timeval tv;
    tv.tv_sec  = cfg_.connect_timeout_ms / 1000;
    tv.tv_usec = (cfg_.connect_timeout_ms % 1000) * 1000;

#ifdef _WIN32
    int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
    int sel = select((int)sock_ + 1, nullptr, &wfds, nullptr, &tv);
#endif
    if (sel <= 0) {
        disconnect();
        throw std::runtime_error("db: connection timeout to " + cfg_.host + ":" + std::to_string(cfg_.port));
    }

    int err = 0;
#ifdef _WIN32
    int len = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
    socklen_t len = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    if (err != 0) {
        disconnect();
        throw std::runtime_error("db: cannot connect to " + cfg_.host + ":" + std::to_string(cfg_.port));
    }

    // Blocking mode geri al
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

    set_socket_timeout(cfg_.query_timeout_ms);
    do_handshake(cfg_.user, cfg_.password, cfg_.database);
    // NO_BACKSLASH_ESCAPES modunu devre dışı bırak — aksi hâlde escape() güvensiz olur
    try { query("SET SESSION sql_mode = REPLACE(@@SESSION.sql_mode, 'NO_BACKSLASH_ESCAPES', '')"); }
    catch (...) {}
}

// ── Ping ──────────────────────────────────────────────────────────────────────

bool MySQLClient::ping() {
    if (sock_ == SOCK_INVALID) return false;
    try {
        std::vector<uint8_t> pkt = { 0x0E };
        send_packet(pkt, 0);
        uint8_t seq;
        auto resp = read_packet(seq);
        return !resp.empty() && resp[0] == 0x00;
    } catch (...) { return false; }
}

// ── ensure_connected ──────────────────────────────────────────────────────────

void MySQLClient::ensure_connected() {
    if (sock_ != SOCK_INVALID) return;  // socket alive — skip round-trip ping
    for (int attempt = 0; attempt < cfg_.max_reconnect; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.reconnect_delay_ms));
        try { do_connect(); return; } catch (...) {}
    }
    throw std::runtime_error("db: connection lost and reconnect failed after " +
                             std::to_string(cfg_.max_reconnect) + " attempts");
}

// ── Packet I/O ────────────────────────────────────────────────────────────────

bool MySQLClient::recv_bytes(uint8_t* buf, size_t len) {
    size_t received = 0;

#ifndef _WIN32
    // ── TLS path: bloklayan SSL_read ──────────────────────────────────────────
    // TLS aktifse fiber non-blocking optimizasyonunu ATLA (SSL_read + SSL_ERROR_WANT_*
    // + fiber yield karmaşık; TLS remote-DB'dir, latency ağ-baskın → düz blocking doğru
    // ve yeterli). Localhost hızlı yolu (non-TLS) fiber path'te korunur.
    if (ssl_) {
        SSL* s = (SSL*)ssl_;
        while (received < len) {
            int r = SSL_read(s, buf + received, (int)(len - received));
            if (r <= 0) { disconnect(); return false; }
            received += (size_t)r;
        }
        return true;
    }
#endif

    // ── Fiber path: non-blocking recv + cooperative yield ─────────────────────
    if (Fiber* fiber = Fiber::current()) {
        while (received < len) {
            int r = ::recv(sock_, (char*)(buf + received),
                           (int)(len - received), MSG_DONTWAIT);
            if (r > 0) { received += r; continue; }
            if (r == 0) { disconnect(); return false; }

            int e = errno;
            if (e != EAGAIN && e != EWOULDBLOCK) { disconnect(); return false; }

#ifndef _WIN32
            // ── Fiber v3: suspend'den önce kısa poll ─────────────────────────
            // Teşhis (Little's law analizi): suspend+epoll-kayıt+eventfd+resume
            // zinciri istek başına ~1.1ms yiyor ve throughput'u ~7k'da tavanlıyor.
            // Localhost DB yanıtı çoğu kez <1ms'de gelir — 1ms'lik poll ile
            // suspension'ların büyük kısmı hiç yaşanmaz. Poll boş dönerse
            // (gerçekten yavaş sorgu) normal suspend yoluna düşülür.
            {
                struct pollfd pfd{};
                pfd.fd     = (int)sock_;
                pfd.events = POLLIN;
                if (::poll(&pfd, 1, 1 /*ms*/) > 0) continue;  // veri geldi — recv'e dön
            }
#endif

            FiberScheduler* sched = get_thread_scheduler();

            if (sched) {
                auto sp = fiber->shared_from_this_fiber();
                // wait_readable: register sock_ with scheduler's epoll,
                // yield this fiber, resume when fd is readable.
                // Returns false on Windows (no epoll) — fall through to blocking recv.
                if (sp && sched->wait_readable(sp, (int)sock_)) {
                    // Resumed: fd is readable, retry recv at top of while loop
                } else {
                    // Not supported or weak_self_ missing — blocking recv
                    int rb = ::recv(sock_, (char*)(buf + received),
                                    (int)(len - received), 0);
                    if (rb <= 0) { disconnect(); return false; }
                    received += rb;
                }
            } else {
                // No fiber scheduler — blocking recv
                int rb = ::recv(sock_, (char*)(buf + received),
                                (int)(len - received), 0);
                if (rb <= 0) { disconnect(); return false; }
                received += rb;
            }
        }
        return true;
    }

    // ── Thread path: blocking recv ────────────────────────────────────────────
    while (received < len) {
        int r = ::recv(sock_, (char*)(buf + received), (int)(len - received), 0);
        if (r <= 0) { disconnect(); return false; }
        received += r;
    }
    return true;
}

void MySQLClient::send_bytes(const uint8_t* buf, size_t len) {
    size_t sent = 0;
#ifndef _WIN32
    if (ssl_) {
        SSL* s = (SSL*)ssl_;
        while (sent < len) {
            int r = SSL_write(s, buf + sent, (int)(len - sent));
            if (r <= 0) { disconnect(); throw std::runtime_error("db: SSL send failed — connection lost"); }
            sent += (size_t)r;
        }
        return;
    }
#endif
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), 0);
        if (r <= 0) { disconnect(); throw std::runtime_error("db: send failed — connection lost"); }
        sent += r;
    }
}

std::vector<uint8_t> MySQLClient::read_packet(uint8_t& seq) {
    uint8_t hdr[4];
    if (!recv_bytes(hdr, 4))
        throw std::runtime_error("db: connection lost or query timeout");
    uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16);
    seq = hdr[3];
    std::vector<uint8_t> payload(len);
    if (len > 0 && !recv_bytes(payload.data(), len))
        throw std::runtime_error("db: connection lost reading payload");
    return payload;
}

void MySQLClient::send_packet(const std::vector<uint8_t>& data, uint8_t seq) {
    uint32_t len = (uint32_t)data.size();
    uint8_t hdr[4] = {(uint8_t)len,(uint8_t)(len>>8),(uint8_t)(len>>16),seq};
    send_bytes(hdr, 4);
    if (!data.empty()) send_bytes(data.data(), data.size());
}

// ── MySQL authentication ───────────────────────────────────────────────────────

// ── caching_sha2_password (MySQL 8.0+ VARSAYILAN eklentisi) ──────────────────
//
// ESKİ DURUM: yalnızca `mysql_native_password` uygulanmıştı ve eklenti adı
// handshake yanıtına SABİT yazılıyordu. Sunucunun bildirdiği eklenti okunmuyor,
// AuthSwitchRequest (0xFE) hiç ele alınmıyordu. Sonuç:
//   MySQL 5.7 / MariaDB  → çalışıyor
//   MySQL 8.0 (2018'den beri VARSAYILAN caching_sha2_password) → BAĞLANAMIYOR
//   MySQL 8.4 (native eklenti kapalı) / 9.x (kaldırıldı)       → BAĞLANAMIYOR
// Yani dil pratikte MySQL 5.7 dilindeydi.
//
// Hızlı yol:  XOR( SHA256(şifre), SHA256( SHA256(SHA256(şifre)) + nonce ) )
// Tam yol:    sunucu 0x04 ister → TLS yoksa sunucudan RSA açık anahtarı istenir
//             (0x02) ve (şifre+NUL) XOR nonce, RSA-OAEP ile şifrelenir.
// Taze sunucuda önbellek boş olduğu için İLK bağlantı daima tam yoldan geçer.
//
// Kripto OpenSSL'den alınıyor — kod tabanında SHA-256'nın 8 ayrı kopyası var
// (S2 kümesi, haritada kayıtlı); dokuzuncuyu eklemiyoruz.
#ifndef _WIN32
static std::vector<uint8_t> caching_sha2_scramble(const std::string& password,
                                                  const std::string& nonce) {
    if (password.empty()) return {};
    unsigned char d1[SHA256_DIGEST_LENGTH], d2[SHA256_DIGEST_LENGTH], d3[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(password.data()), password.size(), d1);
    SHA256(d1, sizeof(d1), d2);
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, d2, sizeof(d2));
    SHA256_Update(&ctx, nonce.data(), nonce.size());
    SHA256_Final(d3, &ctx);
    std::vector<uint8_t> out(SHA256_DIGEST_LENGTH);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) out[i] = d1[i] ^ d3[i];
    return out;
}

// (şifre + NUL) XOR nonce → sunucunun RSA açık anahtarıyla OAEP şifrele.
static std::vector<uint8_t> caching_sha2_rsa_encrypt(const std::string& password,
                                                     const std::string& nonce,
                                                     const std::string& pem) {
    std::string buf = password;
    buf.push_back('\0');
    if (!nonce.empty())
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = (char)(buf[i] ^ nonce[i % nonce.size()]);

    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) throw std::runtime_error("db mysql: RSA anahtarı okunamadı (BIO)");
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) throw std::runtime_error("db mysql: sunucunun RSA açık anahtarı ayrıştırılamadı");

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    std::vector<uint8_t> out;
    size_t outlen = 0;
    bool ok = ctx && EVP_PKEY_encrypt_init(ctx) > 0 &&
              EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
              EVP_PKEY_encrypt(ctx, nullptr, &outlen,
                               reinterpret_cast<const unsigned char*>(buf.data()), buf.size()) > 0;
    if (ok) {
        out.resize(outlen);
        ok = EVP_PKEY_encrypt(ctx, out.data(), &outlen,
                              reinterpret_cast<const unsigned char*>(buf.data()), buf.size()) > 0;
        out.resize(ok ? outlen : 0);
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    if (!ok) throw std::runtime_error("db mysql: RSA şifreleme başarısız");
    return out;
}
#endif // !_WIN32

std::vector<uint8_t> MySQLClient::native_password(const std::string& password,
                                                    const std::string& challenge) {
    if (password.empty()) return {};
    std::vector<uint8_t> pwd(password.begin(), password.end());
    auto stage1 = sha1(pwd);
    auto stage2 = sha1(stage1);
    std::vector<uint8_t> combined(challenge.begin(), challenge.end());
    combined.insert(combined.end(), stage2.begin(), stage2.end());
    auto stage3 = sha1(combined);
    std::vector<uint8_t> token(20);
    for (int i = 0; i < 20; i++) token[i] = stage1[i] ^ stage3[i];
    return token;
}

void MySQLClient::do_handshake(const std::string& user,
                                const std::string& password,
                                const std::string& database) {
    uint8_t seq;
    auto pkt = read_packet(seq);
    if (pkt.empty()) throw std::runtime_error("db: boş handshake paketi");
    const uint8_t* p = pkt.data();
    const uint8_t* end = p + pkt.size();

    if (*p == 0xFF) { p++; uint16_t e=(p+2<=end)?read_u16(p):0; throw std::runtime_error("db: server error "+std::to_string(e)); }

    p++; // protocol version
    while (p < end && *p) p++; if (p < end) p++; // server version (null-terminated)
    // Sınır-güvenli: kısa/kötü niyetli handshake paketi kontrolsüz okumalarla
    // (challenge 8 bayt, auth_len, cap flags) buffer sonrasını overread edebilirdi
    // (MITM/kötü sunucu — TLS yok). Sabit blok = thread(4)+challenge(8)+filler(1)+
    // cap1(2)+charset+status(3)+cap2(2)+auth_len(1)+reserved(10) = 31 bayt.
    if (end - p < 31) throw std::runtime_error("db: bozuk handshake paketi (kısa)");
    p += 4; // thread id

    std::string challenge(p, p + 8); p += 8;
    // filler(1) + capability_flags_1(2) + charset(1) + status_flags(2) + capability_flags_2(2)
    p++;           // filler
    read_u16(p);   // capability_flags_1 (advances p by 2)
    p += 3;        // charset + status_flags
    read_u16(p);   // capability_flags_2 (advances p by 2)
    // p is now at auth_plugin_data_len
    uint8_t auth_len = *p++; // read auth_plugin_data_len
    p += 10;       // skip 10 reserved bytes

    int part2_len = ((int)auth_len - 8) > 13 ? (int)auth_len - 8 : 13;
    if (p + part2_len <= end) { challenge += std::string(p, p + part2_len - 1); p += part2_len; }

    // Sunucunun bildirdiği kimlik doğrulama eklentisi (CLIENT_PLUGIN_AUTH ile gelir).
    // ESKİ HATA: bu alan HİÇ okunmuyordu; cevap her zaman mysql_native_password
    // olarak gönderiliyordu. MySQL 8'in varsayılanı caching_sha2_password olduğu
    // için bağlantı kurulamıyordu.
    std::string srv_plugin;
    while (p < end && *p) srv_plugin.push_back((char)*p++);
    if (srv_plugin.empty()) srv_plugin = "mysql_native_password";

    auto make_token = [&](const std::string& plugin, const std::string& nonce)
                      -> std::vector<uint8_t> {
        if (plugin == "mysql_native_password") return native_password(password, nonce);
        if (plugin == "caching_sha2_password") {
#ifndef _WIN32
            return caching_sha2_scramble(password, nonce);
#else
            throw std::runtime_error(
                "db mysql: caching_sha2_password bu yapida desteklenmiyor (Windows yapisi "
                "OpenSSL'siz derlenir). Cozum: kullaniciyi mysql_native_password ile "
                "olusturun ya da Linux yapisini kullanin.");
#endif
        }
        throw std::runtime_error("db mysql: desteklenmeyen kimlik dogrulama eklentisi: " + plugin);
    };

    std::string cur_plugin = srv_plugin;
    uint32_t client_flags = 0x000FA685;
    if (!database.empty()) client_flags |= 0x00000008;

    uint8_t auth_seq = 1;
    if (cfg_.tls) {
#ifndef _WIN32
        // ── MySQL SSLRequest → TLS upgrade (SIRA KRİTİK) ─────────────────────────
        // Sıra: initial handshake (yukarıda okundu, plaintext) → SSLRequest (CLIENT_SSL
        // flag, auth verisi YOK, seq 1) → SSL_connect (upgrade) → auth artık TLS üstünde
        // (seq 2). Auth KESİNLİKLE SSL_connect'ten SONRA gönderilir (ssl_ set edilince
        // send_bytes SSL_write'a geçer) — aksi halde credentials plaintext sızardı.
        client_flags |= 0x00000800;  // CLIENT_SSL
        std::vector<uint8_t> ssl_req;
        ssl_req.push_back(client_flags&0xFF); ssl_req.push_back((client_flags>>8)&0xFF);
        ssl_req.push_back((client_flags>>16)&0xFF); ssl_req.push_back((client_flags>>24)&0xFF);
        ssl_req.insert(ssl_req.end(), {0xFF,0xFF,0xFF,0x00}); // max packet size
        ssl_req.push_back(45);                                // charset (utf8mb4)
        ssl_req.insert(ssl_req.end(), 23, 0);                 // reserved
        send_packet(ssl_req, 1);                              // plaintext — henüz ssl_ yok
        SSL_CTX* ctx = mysql_ssl_ctx();
        if (!ctx) throw std::runtime_error("db mysql: SSL_CTX oluşturulamadı (TLS)");
        SSL* s = SSL_new(ctx);
        if (!s) throw std::runtime_error("db mysql: SSL_new başarısız (TLS)");
        SSL_set_fd(s, (int)sock_);
        if (SSL_connect(s) != 1) {
            unsigned long e = ERR_get_error();
            char eb[256]; ERR_error_string_n(e, eb, sizeof(eb));
            SSL_free(s);
            throw std::runtime_error(std::string("db mysql: TLS handshake başarısız: ") + eb);
        }
        ssl_     = s;   // bundan sonra send/recv_bytes SSL üstünden (auth dahil)
        auth_seq = 2;   // auth paketi TLS içinde, SSLRequest'ten sonraki seq
#else
        throw std::runtime_error("db mysql: TLS (mysqls://) bu yapida desteklenmiyor "
                                 "(Windows yapisi OpenSSL'siz derlenir). Linux yapisini kullanin.");
#endif
    }

    auto token = make_token(cur_plugin, challenge);

    std::vector<uint8_t> auth_pkt;
    auth_pkt.push_back(client_flags&0xFF); auth_pkt.push_back((client_flags>>8)&0xFF);
    auth_pkt.push_back((client_flags>>16)&0xFF); auth_pkt.push_back((client_flags>>24)&0xFF);
    auth_pkt.insert(auth_pkt.end(), {0xFF,0xFF,0xFF,0x00});
    auth_pkt.push_back(45);
    auth_pkt.insert(auth_pkt.end(), 23, 0);
    auth_pkt.insert(auth_pkt.end(), user.begin(), user.end()); auth_pkt.push_back(0);
    auth_pkt.push_back((uint8_t)token.size());
    auth_pkt.insert(auth_pkt.end(), token.begin(), token.end());
    if (!database.empty()) { auth_pkt.insert(auth_pkt.end(), database.begin(), database.end()); auth_pkt.push_back(0); }
    auth_pkt.insert(auth_pkt.end(), cur_plugin.begin(), cur_plugin.end()); auth_pkt.push_back(0);

    send_packet(auth_pkt, auth_seq);

    // Kimlik doğrulama diyaloğu. ESKİ HATA: yalnızca 0xFF (hata) kontrol ediliyor,
    // 0xFE (AuthSwitchRequest) ve 0x01 (AuthMoreData) HİÇ ele alınmıyordu — oysa
    // CLIENT_PLUGIN_AUTH bayrağı set edildiği için sunucu bunları göndermeye
    // yetkiliydi. MySQL 8 tam olarak bunu yapıyor ve diyalog kopuyordu.
    auto auth_error = [&](const std::vector<uint8_t>& r) {
        std::string msg = "db: authentication failed";
        if (r.size() > 3) {
            size_t off = 3;
            if (r[off] == '#') off += 6;
            msg = "db: " + std::string(r.begin() + off, r.end());
        }
        throw std::runtime_error(msg);
    };

    for (int round = 0; round < 5; ++round) {   // sonsuz diyaloğa karşı tavan
        auto resp = read_packet(seq);
        if (resp.empty()) throw std::runtime_error("db: boş kimlik doğrulama yanıtı");
        if (resp[0] == 0xFF) auth_error(resp);
        if (resp[0] == 0x00) return;            // OK — doğrulandı

        if (resp[0] == 0xFE) {
            // AuthSwitchRequest: [0xFE][plugin adı NUL][yeni nonce]
            size_t i = 1;
            std::string np;
            while (i < resp.size() && resp[i]) np.push_back((char)resp[i++]);
            ++i;
            std::string nonce;
            while (i < resp.size() && resp[i]) nonce.push_back((char)resp[i++]);
            cur_plugin = np.empty() ? cur_plugin : np;
            challenge  = nonce;
            auto t = make_token(cur_plugin, challenge);
            send_packet(t, (uint8_t)(seq + 1));
            continue;
        }

        if (resp[0] == 0x01) {
            // AuthMoreData — caching_sha2_password akışı
            uint8_t code = resp.size() > 1 ? resp[1] : 0;
            if (code == 0x03) continue;         // fast auth başarılı → sıradaki paket OK
            if (code == 0x04) {
                // Tam kimlik doğrulama (önbellek boş → ilk bağlantı).
#ifndef _WIN32
                if (ssl_) {
                    // TLS AKTİF → kanal zaten şifreli; şifre CLEARTEXT (NUL-sonlu) gönderilir.
                    // RSA açık-anahtar dansı YAPILMAZ (sunucu TLS'te RSA vermez → eski kod
                    // "RSA açık anahtarı vermedi" ile çökerdi). MySQL protokolü tam bunu ister.
                    std::vector<uint8_t> cleartext(password.begin(), password.end());
                    cleartext.push_back(0x00);
                    send_packet(cleartext, (uint8_t)(seq + 1));
                    continue;
                }
#endif
                // TLS yok → sunucudan RSA açık anahtarı istenir (0x02), şifre RSA-OAEP ile.
#ifdef _WIN32
                throw std::runtime_error(
                    "db mysql: caching_sha2_password tam kimlik dogrulamasi bu yapida "
                    "desteklenmiyor (Windows yapisi OpenSSL'siz derlenir). Cozum: "
                    "kullaniciyi mysql_native_password ile olusturun ya da Linux yapisini kullanin.");
#else
                send_packet(std::vector<uint8_t>{0x02}, (uint8_t)(seq + 1));
                auto keypkt = read_packet(seq);
                if (keypkt.size() < 2 || keypkt[0] != 0x01)
                    throw std::runtime_error("db mysql: sunucu RSA açık anahtarı vermedi");
                std::string pem(keypkt.begin() + 1, keypkt.end());
                auto enc = caching_sha2_rsa_encrypt(password, challenge, pem);
                send_packet(enc, (uint8_t)(seq + 1));
                continue;
#endif
            }
            throw std::runtime_error("db mysql: beklenmeyen kimlik dogrulama verisi (0x" +
                                     std::to_string((int)code) + ")");
        }
        throw std::runtime_error("db mysql: beklenmeyen kimlik dogrulama paketi");
    }
    throw std::runtime_error("db mysql: kimlik dogrulama tamamlanmadi (tur siniri asildi)");
}

// ── Query ─────────────────────────────────────────────────────────────────────

std::vector<DbRow> MySQLClient::query(const std::string& sql) {
    ensure_connected();
    auto start = std::chrono::steady_clock::now();

    std::vector<uint8_t> pkt;
    pkt.push_back(0x03);
    pkt.insert(pkt.end(), sql.begin(), sql.end());

    try { send_packet(pkt, 0); } catch (...) { do_connect(); send_packet(pkt, 0); }

    uint8_t seq;
    auto resp = read_packet(seq);
    last_query_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start).count();

    if (resp.empty()) throw std::runtime_error("db: empty response");
    const uint8_t* p = resp.data();
    const uint8_t* end = p + resp.size();

    if (resp[0] == 0xFF) {
        p++; uint16_t code=read_u16(p);
        if (p < end && *p == '#') p += 6;
        throw std::runtime_error("db: query error " + std::to_string(code) + ": " + std::string(p, end));
    }
    if (resp[0] == 0x00) {
        p++; affected_rows_=read_lenenc(p,end); last_insert_id_=read_lenenc(p,end);
        return {};
    }

    uint64_t col_count = read_lenenc(p, end);
    // Sunucu-verili col_count sanity cap — kötü niyetli sunucu 2^64 verirse
    // kolon-okuma döngüsü kaynak tüketir/asılır. Gerçek query'ler ≪65535 sütun.
    if (col_count > MYSQL_MAX_COLUMNS)
        throw std::runtime_error("db mysql: sütun sayısı sınırı aşıldı (kötü niyetli sunucu?)");
    struct ColInfo { std::string name; uint8_t type; };
    std::vector<ColInfo> columns;
    for (uint64_t i = 0; i < col_count; i++) {
        auto col_pkt = read_packet(seq);
        const uint8_t* cp = col_pkt.data();
        const uint8_t* ce = cp + col_pkt.size();
        read_lenenc_str(cp,ce);                   // catalog
        read_lenenc_str(cp,ce);                   // schema
        read_lenenc_str(cp,ce);                   // table
        read_lenenc_str(cp,ce);                   // org_table
        std::string name = read_lenenc_str(cp,ce); // name
        read_lenenc_str(cp,ce);                   // org_name
        if (cp < ce) cp++;                        // 0x0C filler byte
        if (cp + 2 <= ce) cp += 2;               // charset (2 bytes)
        if (cp + 4 <= ce) cp += 4;               // col_length (4 bytes)
        uint8_t col_type = (cp < ce) ? *cp : 0xFE;
        columns.push_back({std::move(name), col_type});
    }
    read_packet(seq); // EOF packet

    std::vector<DbRow> rows;
    while (true) {
        auto row_pkt = read_packet(seq);
        if (row_pkt.empty() || row_pkt[0]==0xFE || row_pkt[0]==0xFF) break;
        DbRow row;
        const uint8_t* rp = row_pkt.data();
        const uint8_t* re = rp + row_pkt.size();
        for (const auto& col : columns) {
            DbValue dv;
            if (rp < re && *rp == 0xFB) {
                rp++;             // NULL marker'ı tüket
                dv.is_null = true;
            } else {
                dv.str  = read_lenenc_str(rp, re);
                dv.type = col.type;
            }
            row.push_back({col.name, std::move(dv)});
        }
        rows.push_back(row);
    }
    return rows;
}

// ── Prepared Statements (COM_STMT_PREPARE + COM_STMT_EXECUTE) ────────────────

MySQLClient::StmtMeta MySQLClient::stmt_prepare(const std::string& sql) {
    ensure_connected();
    std::vector<uint8_t> pkt;
    pkt.push_back(0x16); // COM_STMT_PREPARE
    pkt.insert(pkt.end(), sql.begin(), sql.end());
    send_packet(pkt, 0);

    uint8_t seq;
    auto resp = read_packet(seq);
    if (resp.empty()) throw std::runtime_error("db: stmt_prepare empty response");
    if (resp[0] == 0xFF) {
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        // Sınır-güvenli: kısa 0xFF paketinde (sadece FF) memcpy 2-bayt overread
        // yapıp p'yi end'in ötesine taşırdı → std::string(p,end) UB. Kötü niyetli/
        // MITM sunucu tetikler. error-code (2) ve '#SQLSTATE' (6) sınır-kontrollü atla.
        if (p + 2 <= end) p += 2; else p = end;             // error code (kullanılmıyor)
        if (p < end && *p == '#' && p + 6 <= end) p += 6;   // '#SQLSTATE' işareti
        throw std::runtime_error("db: " + std::string(p, end));
    }
    if (resp[0] != 0x00 || resp.size() < 12)
        throw std::runtime_error("db: malformed STMT_PREPARE response");

    const uint8_t* p = resp.data() + 1;
    uint32_t stmt_id; memcpy(&stmt_id, p, 4); p += 4;
    uint16_t num_cols;   memcpy(&num_cols,   p, 2); p += 2;
    uint16_t num_params; memcpy(&num_params, p, 2);

    // param defs + EOF
    for (int i = 0; i < num_params; i++) read_packet(seq);
    if (num_params > 0) read_packet(seq);
    // col defs + EOF
    for (int i = 0; i < num_cols; i++) read_packet(seq);
    if (num_cols > 0) read_packet(seq);

    return {stmt_id, num_cols, num_params};
}

void MySQLClient::stmt_close(uint32_t stmt_id) {
    std::vector<uint8_t> pkt;
    pkt.push_back(0x19); // COM_STMT_CLOSE
    pkt.push_back(stmt_id & 0xFF); pkt.push_back((stmt_id>>8) & 0xFF);
    pkt.push_back((stmt_id>>16) & 0xFF); pkt.push_back((stmt_id>>24) & 0xFF);
    try { send_packet(pkt, 0); } catch (...) {}
}

std::vector<DbRow> MySQLClient::stmt_execute(const StmtMeta& m,
                                              const std::vector<DbParam>& params) {
    // max_allowed_packet koruması: varsayılan 64 MB
    static constexpr size_t MYSQL_MAX_PACKET = 64 * 1024 * 1024;
    size_t total_str = 0;
    for (const auto& p : params)
        if (p.kind == DbParam::TEXT_VAL) total_str += p.s.size();
    if (total_str > MYSQL_MAX_PACKET)
        throw std::runtime_error("db mysql: parametre toplam boyutu max_allowed_packet limitini aşıyor (64 MB)");

    int n = (int)params.size();
    std::vector<uint8_t> pkt;
    pkt.push_back(0x17); // COM_STMT_EXECUTE
    auto push_le32 = [&](uint32_t v) {
        for (int b=0;b<4;b++) pkt.push_back((v>>(b*8))&0xFF);
    };
    push_le32(m.id);
    pkt.push_back(0x00); // flags
    push_le32(1); // iteration count

    if (n > 0) {
        // NULL bitmap
        int nbytes = (n + 7) / 8;
        std::vector<uint8_t> nbm(nbytes, 0);
        for (int i = 0; i < n; i++)
            if (params[i].kind == DbParam::NULL_VAL) nbm[i/8] |= (1 << (i%8));
        pkt.insert(pkt.end(), nbm.begin(), nbm.end());
        pkt.push_back(0x01); // new_params_bound_flag

        // type codes (2 bytes each)
        for (int i = 0; i < n; i++) {
            uint8_t tc = 0xFE; // BLOB → string
            if (params[i].kind == DbParam::INT_VAL)   tc = 0x08; // LONGLONG
            if (params[i].kind == DbParam::FLOAT_VAL) tc = 0x05; // DOUBLE
            if (params[i].kind == DbParam::BOOL_VAL)  tc = 0x10; // TINY
            pkt.push_back(tc); pkt.push_back(0x00); // unsigned_flag=0
        }

        // values
        for (int i = 0; i < n; i++) {
            if (params[i].kind == DbParam::NULL_VAL) continue;
            switch (params[i].kind) {
                case DbParam::INT_VAL: {
                    uint64_t v; memcpy(&v, &params[i].i, 8);
                    for (int b=0;b<8;b++) pkt.push_back((v>>(b*8))&0xFF);
                    break;
                }
                case DbParam::FLOAT_VAL: {
                    uint64_t v; memcpy(&v, &params[i].d, 8);
                    for (int b=0;b<8;b++) pkt.push_back((v>>(b*8))&0xFF);
                    break;
                }
                case DbParam::BOOL_VAL:
                    pkt.push_back(params[i].b ? 1 : 0);
                    break;
                default: { // TEXT
                    const std::string& s = params[i].s;
                    uint64_t len = s.size();
                    if (len < 251)       { pkt.push_back((uint8_t)len); }
                    else if (len < 65536){ pkt.push_back(0xFC); pkt.push_back(len&0xFF); pkt.push_back((len>>8)&0xFF); }
                    else                 { pkt.push_back(0xFD); for(int b=0;b<3;b++) pkt.push_back((len>>(b*8))&0xFF); }
                    pkt.insert(pkt.end(), s.begin(), s.end());
                    break;
                }
            }
        }
    } else {
        pkt.push_back(0x01); // new_params_bound_flag (no params)
    }

    send_packet(pkt, 0);

    uint8_t seq;
    auto resp = read_packet(seq);
    if (resp.empty()) throw std::runtime_error("db: stmt_execute empty response");
    if (resp[0] == 0xFF) {
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        // Sınır-güvenli (stmt_prepare error-path ile aynı): kısa 0xFF paketinde
        // memcpy 2-bayt overread + std::string(p,end) UB'sini önle.
        if (p + 2 <= end) p += 2; else p = end;             // error code (kullanılmıyor)
        if (p < end && *p == '#' && p + 6 <= end) p += 6;   // '#SQLSTATE'
        throw std::runtime_error("db: " + std::string(p, end));
    }
    if (resp[0] == 0x00) { // OK (INSERT/UPDATE/DELETE)
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        affected_rows_  = read_lenenc(p, end);
        last_insert_id_ = read_lenenc(p, end);
        return {};
    }

    // Binary resultset
    const uint8_t* p = resp.data();
    const uint8_t* end = p + resp.size();
    uint64_t col_count = read_lenenc(p, end);
    // Sanity cap — kötü niyetli sunucu 2^64 col_count verirse döngü + `(int)col_count`
    // null-bitmap boyutu (satır ~657) taşar/kaynak tüketir.
    if (col_count > MYSQL_MAX_COLUMNS)
        throw std::runtime_error("db mysql: sütun sayısı sınırı aşıldı (kötü niyetli sunucu?)");

    struct ColInfo { std::string name; uint8_t type; };
    std::vector<ColInfo> columns;
    for (uint64_t i = 0; i < col_count; i++) {
        auto col_pkt = read_packet(seq);
        const uint8_t* cp = col_pkt.data();
        const uint8_t* ce = cp + col_pkt.size();
        read_lenenc_str(cp,ce); read_lenenc_str(cp,ce); // catalog, schema
        read_lenenc_str(cp,ce); read_lenenc_str(cp,ce); // table, org_table
        std::string name = read_lenenc_str(cp,ce);
        read_lenenc_str(cp,ce); // org_name
        if (cp<ce) cp++;       // filler
        if (cp+2<=ce) cp+=2;   // charset
        if (cp+4<=ce) cp+=4;   // col_len
        uint8_t col_type = (cp<ce) ? *cp : 0xFE;
        columns.push_back({std::move(name), col_type});
    }
    read_packet(seq); // EOF

    std::vector<DbRow> rows;
    while (true) {
        auto row_pkt = read_packet(seq);
        if (row_pkt.empty() || row_pkt[0]==0xFE || row_pkt[0]==0xFF) break;

        // binary row: 0x00 header + null_bitmap
        const uint8_t* rp = row_pkt.data();
        const uint8_t* re = rp + row_pkt.size();
        if (rp < re) rp++; // skip 0x00
        int nb = ((int)col_count + 7 + 2) / 8;
        std::vector<uint8_t> nbm(nb, 0);
        for (int i=0; i<nb && rp<re; i++) nbm[i] = *rp++;

        DbRow row;
        for (size_t ci = 0; ci < col_count; ci++) {
            bool is_null = (nbm[(ci+2)/8] >> ((ci+2)%8)) & 1;
            DbValue dv; dv.type = columns[ci].type;
            if (is_null) { dv.is_null = true; }
            else {
                uint8_t ct = columns[ci].type;
                auto read_le = [&](int bytes) -> int64_t {
                    uint64_t v = 0;
                    for (int b=0; b<bytes && rp<re; b++,rp++) v |= ((uint64_t)*rp << (b*8));
                    return (int64_t)v;
                };
                switch(ct) {
                    case 0x01:           dv.str = std::to_string(read_le(1)); break;
                    case 0x02: case 0x0D: dv.str = std::to_string(read_le(2)); break;
                    case 0x03: case 0x09: dv.str = std::to_string(read_le(4)); break;
                    case 0x08:            dv.str = std::to_string(read_le(8)); break;
                    case 0x04: { float f; if(rp+4<=re){memcpy(&f,rp,4);rp+=4;} dv.str=std::to_string(f); break; }
                    case 0x05: { double d; if(rp+8<=re){memcpy(&d,rp,8);rp+=8;} dv.str=std::to_string(d); break; }
                    case 0x10:            dv.str = std::to_string(read_le(1)); break;
                    default:              dv.str = read_lenenc_str(rp, re); break;
                }
            }
            row.push_back({columns[ci].name, std::move(dv)});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<DbRow> MySQLClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    ensure_connected();
    auto meta = stmt_prepare(sql);
    auto rows = stmt_execute(meta, params);
    stmt_close(meta.id);
    return rows;
}

// ── Escape ────────────────────────────────────────────────────────────────────

// Tırnak kaçışı `''` (STANDART SQL) — `\'` DEĞİL.
//
// ESKİ HATA: `'` → `\'` yazılıyordu. MySQL `NO_BACKSLASH_ESCAPES` modunda ters
// bölüyü kaçış karakteri SAYMAZ; o modda `\'` = "ters bölü + tırnak" demektir →
// TIRNAK KAPANIR → SQL enjeksiyonu. ÖLÇÜLDÜ (gerçek sunucu, oturum o moda
// alınarak): `' OR 1=1 -- ` yükü **tüm tabloyu** döndürdü (3/3 satır).
//
// Bunu tutan tek şey do_connect()'teki `SET SESSION sql_mode = REPLACE(...)`
// komutunun BAŞARILI olmasıydı. Bu yeterli bir güvenlik zemini değil:
//   * ProxySQL / bağlantı çoklayıcıları arka uç bağlantılarını multiplex eder —
//     SET başarılı olur, sonra oturum durumu sessizce kaybolabilir; bağlantı
//     bozulmaz, yalnızca koruma yok olur.
//   * Bir ifadenin reddedilmesi bağlantının bozuk olduğu anlamına gelmez
//     (vekil filtresi, audit eklentisi, governor tek ifadeyi reddedebilir).
// Asimetri belirleyici: düzeltme tek satır, yanılmanın bedeli SQL enjeksiyonu.
//
// `''` HER İKİ modda da güvenlidir (standart SQL; MySQL normal modda da kabul
// eder) → güvenlik artık `SET`'in başarısına BAĞLI DEĞİL. PostgreSQL tarafı
// zaten `''` kullanıyordu; bu değişiklik iki sürücüyü aynı zemine getirir.
//
// `SET SESSION` KALIYOR — ama artık güvenlik için değil, VERİ SADAKATİ için:
// NBE modunda aşağıdaki `\\` ikilemesi veriyi bozar (iki ters bölü saklanır).
// O bir doğruluk meselesi, güvenlik değil.
std::string MySQLClient::escape(const std::string& s) {
    std::string out; out.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\'': out += "''";   break;   // standart SQL — NBE modunda da güvenli
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case 0:    out += "\\0";  break;
            default:   out += c;
        }
    }
    return out;
}

// ── Packet readers ────────────────────────────────────────────────────────────

uint64_t MySQLClient::read_lenenc(const uint8_t*& p, const uint8_t* end) {
    if (p>=end) return 0;
    uint8_t first=*p++;
    if (first<0xFB) return first;
    if (first==0xFC&&p+2<=end){uint64_t v=p[0]|((uint64_t)p[1]<<8);p+=2;return v;}
    if (first==0xFD&&p+3<=end){uint64_t v=p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16);p+=3;return v;}
    if (first==0xFE&&p+8<=end){uint64_t v;memcpy(&v,p,8);p+=8;return v;}
    return 0;
}

std::string MySQLClient::read_lenenc_str(const uint8_t*& p, const uint8_t* end) {
    if (p>=end) return "";
    if (*p==0xFB){p++;return "";}
    uint64_t len=read_lenenc(p,end);
    // TAŞMA-GÜVENLİ: `p+len>end` — len sunucudan gelir ve 0xFE formatında ~2^64
    // olabilir → `p+len` işaretçi taşması (UB) yapıp wrap'lar → kontrol atlanır →
    // ~2^64 baytlık overread/crash. Kötü niyetli/MITM sunucu (TLS yok) tetikler.
    // p<=end invariantı gereği `end-p` güvenli; len'i mevcut bayta clamp'le.
    uint64_t avail = (uint64_t)(end - p);
    if (len > avail) len = avail;
    std::string s(p, p + (size_t)len); p += len;
    return s;
}

uint16_t MySQLClient::read_u16(const uint8_t*& p){uint16_t v=p[0]|(p[1]<<8);p+=2;return v;}
uint32_t MySQLClient::read_u32(const uint8_t*& p){uint32_t v;memcpy(&v,p,4);p+=4;return v;}
uint64_t MySQLClient::read_u64(const uint8_t*& p){uint64_t v;memcpy(&v,p,8);p+=8;return v;}

} // namespace look

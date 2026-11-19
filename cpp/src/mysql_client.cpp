#include "look/mysql_client.h"
#include "look/mysql_wire_parse.h"   // pure parser, single definition (members forward to it)
#include "look/sock_idle.h"         // idle_socket_unusable: liveness probe before sending
#include "look/fiber.h"
#include "look/fiber_io.h"
#include "look/fiber_net.h"   // DNS/connect/TLS/send must not hold the thread inside a fiber
#include "look/format.h"   // look_format_double (the language's single double format)
#include "look/logger.h"
#ifndef _WIN32
// caching_sha2_password (MySQL 8+ default): SHA-256 + RSA-OAEP.
// OpenSSL is already linked (POSIX); on Windows only ws2_32+bcrypt are linked —
// that case is handled explicitly inside do_handshake.
#  include <openssl/sha.h>
#  include <openssl/evp.h>
#  include <openssl/pem.h>
#  include <openssl/rsa.h>
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#include "look/tls_peer.h"   // SNI + name verification (IP -> IP SAN), handshake failure reason
#endif
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
  // MSG_DONTWAIT is POSIX-specific — it does not exist on Windows. Used on the fiber path (fibers are
  // already disabled on Windows). 0 for the build; the socket stays in blocking mode.
  #ifndef MSG_DONTWAIT
    #define MSG_DONTWAIT 0
  #endif
#else
  #include <netinet/tcp.h>
  #include <poll.h>
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0   // Windows/macOS: no flag (there is no SIGPIPE on Windows)
#endif

namespace look {

// Upper bound on the server-supplied column count — against the DoS where a malicious/MITM server
// exhausts resources with a huge col_count. Real queries are far below 65535 columns.
static constexpr uint64_t MYSQL_MAX_COLUMNS = 65535;

#ifndef _WIN32
// ── DB-TLS: client SSL_CTX ───────────────────────────────────────────────────
// In the MySQL protocol, SSL is set up AFTER the initial handshake / BEFORE auth
// (SSLRequest packet -> TLS upgrade -> auth is now encrypted). The goal is wire encryption
// (--ssl-mode=REQUIRED): since DB certificates are often self-signed we do NOT do PEER
// verification (otherwise the connection breaks in a typical setup); it protects against passive
// eavesdropping. Certificate verification could become a separate mode in the future.
static SSL_CTX* mysql_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        look::tls_client_defaults(c);   // at least TLS 1.2, independent of the machine
        if (c) {
            // CTX default: NO verification. verify mode (?tls=verify) is enabled per SSL.
            SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
            // Load the trust store so verify mode can verify against the system CAs
            // (it also reads the SSL_CERT_FILE/DIR env — for static OpenSSL configure_system_ca_bundle
            // is done in http_client; the default paths are enough here).
            SSL_CTX_set_default_verify_paths(c);
        }
        return c;
    }();
    return ctx;
}
#endif

// ── Pure C++ SHA1 — platform independent ────────────────────────────────────────

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
    // COM_QUIT (best effort): tell the server "I am leaving". OLD BEHAVIOR: the socket was closed directly ->
    // MySQL counted every connection as "aborted" and wrote a warning to the error log (ER_NET_READ_ERROR,
    // Aborted_clients — measured: a script running a single query did so for all 16 connections of the pool).
    // If send_bytes fails it calls disconnect() itself -> quitting_ breaks the recursion.
    if (sock_ != SOCK_INVALID && !quitting_) {
        quitting_ = true;
        const uint8_t quit[5] = {1, 0, 0, 0, 0x01};   // length 1, sequence 0, COM_QUIT
        try { send_bytes(quit, sizeof(quit)); } catch (...) {}
        quitting_ = false;
    }
#ifndef _WIN32
    if (ssl_) {
        SSL* s = (SSL*)ssl_;
        SSL_shutdown(s);   // TLS close_notify (best-effort)
        SSL_free(s);       // does not close the socket — sock_ is closed separately
        ssl_ = nullptr;
    }
#endif
    if (sock_ != SOCK_INVALID) {
        close_sock(sock_);
        sock_ = SOCK_INVALID;
    }
    // The session is over: the bytes in the buffer and the statement ids belonged to that session. On a new connection,
    // EXECUTE with the old id would run another statement (or none).
    rbuf_pos_ = rbuf_len_ = 0;
    clear_stmt_cache(false);
}

void MySQLClient::clear_stmt_cache(bool close_on_server) {
    // Take over the map first: if sending stmt_close fails, disconnect() re-enters here
    // (false) and would clear stmt_cache_ in the middle of the loop -> iterator UAF.
    auto old = std::move(stmt_cache_);
    stmt_cache_.clear();
    if (close_on_server)
        for (auto& kv : old) {
            if (sock_ == SOCK_INVALID) break;   // connection lost: the server already released the statements
            stmt_close(kv.second.meta.id);
        }
}

[[noreturn]] void MySQLClient::throw_err_packet(const std::vector<uint8_t>& pkt, const char* prefix) {
    const uint8_t* p = pkt.data() + 1;
    const uint8_t* end = pkt.data() + pkt.size();
    last_error_code_ = 0;
    if (p + 2 <= end) { last_error_code_ = (uint16_t)(p[0] | (p[1] << 8)); p += 2; } else p = end;
    if (p < end && *p == '#' && p + 6 <= end) p += 6;   // '#SQLSTATE'
    throw std::runtime_error(std::string(prefix) + std::string(p, end));
}

// CALL sends at least one more OK after a result set (status flag 0x0008,
// SERVER_MORE_RESULTS_EXISTS). If it is not read it stays on the connection and the NEXT query — another
// request's query in the pool — takes it as its own answer. Everything is read up to the last OK; the first set is returned.
void MySQLClient::drain_more_results(uint16_t status, uint8_t& seq) {
    while (status & 0x0008) {
        auto r = read_packet(seq);
        if (r.empty()) throw std::runtime_error("db: empty response");
        if (r[0] == 0xFF) throw_err_packet(r, "db: ");
        const uint8_t* p = r.data();
        const uint8_t* end = p + r.size();
        if (r[0] == 0x00) {
            p++; read_lenenc(p, end); read_lenenc(p, end);
            status = (end - p >= 2) ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
            in_txn_ = (status & 0x0001) != 0;
            continue;
        }
        uint64_t cols = read_lenenc(p, end);
        if (cols > MYSQL_MAX_COLUMNS)
            throw std::runtime_error("db mysql: column count limit exceeded (malicious server?)");
        for (uint64_t i = 0; i < cols; i++) read_packet(seq);
        read_packet(seq);   // EOF
        status = 0;
        while (true) {
            auto row = read_packet(seq);
            if (!row.empty() && row[0] == 0xFF) throw_err_packet(row, "db: ");
            if (row.empty() || (row[0] == 0xFE && row.size() < 9)) {
                if (row.size() >= 5) { status = (uint16_t)(row[3] | (row[4] << 8)); in_txn_ = (status & 0x0001) != 0; }
                break;
            }
        }
    }
}

// ── Socket timeout ────────────────────────────────────────────────────────────

void MySQLClient::set_socket_timeout(int ms) {
    if (sock_ == SOCK_INVALID) return;
    rcv_tmo_ms_ = ms;   // the fiber wait is bounded by the same duration
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

    // Go net.Dial model (IPv4 + IPv6, try the addresses in order) — look/fiber_net.h tcp_dial.
    // When connecting and resolving names inside a fiber only the fiber waits.
    {
        bool resolved = false, timed_out = false;
        auto s = look::tcp_dial(cfg_.host, cfg_.port, cfg_.connect_timeout_ms, resolved, timed_out);
        if (s == look::DIAL_INVALID) {
            if (!resolved) throw std::runtime_error("db: cannot resolve host: " + cfg_.host);
            throw std::runtime_error((timed_out ? "db: connection timeout to " : "db: cannot connect to ") +
                                     look::join_host_port(cfg_.host, cfg_.port));
        }
        sock_ = s;
    }

    // Disable the Nagle algorithm — avoids a 40ms delay on small packets
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
    // Disable NO_BACKSLASH_ESCAPES mode — otherwise escape() becomes unsafe
    // Only removes NO_BACKSLASH_ESCAPES; it does NOT set strict mode (STRICT_*) — the server's mode is preserved.
    // Escape safety does not depend on this (db::escape doubles '', queries bind real parameters),
    // but the failure used to be swallowed entirely: log it, the connection continues.
    try { query("SET SESSION sql_mode = REPLACE(@@SESSION.sql_mode, 'NO_BACKSLASH_ESCAPES', '')"); }
    catch (const std::exception& e) {
        Logger::instance().log(LogLevel::LOG_WARN, "db",
            std::string("MySQL: could not clear NO_BACKSLASH_ESCAPES from sql_mode (the connection is used as is): ") + e.what());
    }
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
    // If the server closed an idle waiting connection (wait_timeout, restart, KILL), detect it
    // WITHOUT SENDING the query: previously the first query went to the closed socket and failed (the first
    // request of the morning was a 500). A query that has been sent is never repeated — this is only the probe before sending.
    // If unread bytes remain in the read buffer (or the TLS buffer), an earlier answer
    // was left half-read (parse error): everything read on this connection would be one query behind.
    // idle_socket_unusable sees the leftover on the socket; it cannot see what is in the buffer.
    bool unread = rbuf_len_ != rbuf_pos_;
#ifndef _WIN32
    if (ssl_ && SSL_pending((SSL*)ssl_) > 0) unread = true;
#endif
    if (sock_ != SOCK_INVALID && (unread || idle_socket_unusable(sock_))) disconnect();
    if (sock_ != SOCK_INVALID) return;  // socket alive — skip the round-trip ping
    // A connection that drops inside a transaction is NOT replaced with a new one: the server rolled the transaction back;
    // on a new connection the following commands would be written permanently OUTSIDE the transaction (silent inconsistency).
    if (in_txn_) {
        in_txn_ = false;
        throw std::runtime_error("db: the connection was closed inside a transaction; "
                                 "the server rolled the transaction back and nothing of it was kept");
    }
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
    // ── TLS path: SSL_read ────────────────────────────────────────────────────
    // Inside a fiber, if the TLS buffer is empty only the fiber waits until the socket is readable.
    // (Previously there was a plain blocking read here: remote/managed database = TLS, and
    // one slow query held up all requests on that thread.)
    if (ssl_) {
        SSL* s = (SSL*)ssl_;
        while (received < len) {
            if (SSL_pending(s) == 0 && fiber_wait_readable((int)sock_, rcv_tmo_ms_) == 0) {
                disconnect(); return false;
            }
            int r = SSL_read(s, buf + received, (int)(len - received));
            if (r <= 0) { disconnect(); return false; }
            received += (size_t)r;
        }
        return true;
    }
#endif

    // ── Plain TCP: buffered read ───────────────────────────────────────────────
    // One recv fills the buffer, packets (header + body) are served from the buffer. Previously each packet was
    // two recvs (4-byte header, then body): a one-line SELECT took ~30 system calls.
    // A request larger than the buffer (big row/BLOB) is read directly into the destination when the buffer is empty.
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

// From the plain TCP socket at least 1, at most cap bytes. <=0: connection ended (disconnected).
int MySQLClient::recv_some(uint8_t* buf, size_t cap) {
    // ── Fiber path: non-blocking recv + cooperative yield ─────────────────────
    if (Fiber::current()) {
        for (;;) {
            int r = ::recv(sock_, (char*)buf, (int)cap, MSG_DONTWAIT);
            if (r > 0) return r;
            if (r == 0) { disconnect(); return 0; }

            int e = errno;
            if (e != EAGAIN && e != EWOULDBLOCK) { disconnect(); return -1; }

            // Short poll + suspend (see fiber_io.h). The time limit is query_timeout: SO_RCVTIMEO
            // does not work while a fiber waits — previously an unresponsive server held the request indefinitely.
            int w = fiber_wait_readable((int)sock_, rcv_tmo_ms_);
            if (w == 1) continue;              // readable — go back to recv
            if (w == 0) { disconnect(); return -1; }   // time is up
            // No timer / not supported — blocking recv
            int rb = ::recv(sock_, (char*)buf, (int)cap, 0);
            if (rb <= 0) { disconnect(); return -1; }
            return rb;
        }
    }

    // ── Thread path: blocking recv — blocking-ok: no fibers (command line, setup) ──
    int r = ::recv(sock_, (char*)buf, (int)cap, 0);
    if (r <= 0) { disconnect(); return -1; }
    return r;
}

void MySQLClient::send_bytes(const uint8_t* buf, size_t len) {
#ifndef _WIN32
    // If the send buffer is full, only the fiber waits inside a fiber (outside a fiber: SO_SNDTIMEO).
    if (!look::fiber_send_all((int)sock_, (SSL*)ssl_, buf, len, rcv_tmo_ms_)) {
        bool tls = ssl_ != nullptr;
        disconnect();
        throw std::runtime_error(tls ? "db: SSL send failed — connection lost" : "db: send failed — connection lost");
    }
#else
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), MSG_NOSIGNAL);   // No SIGPIPE: EPIPE on a closed socket
        if (r <= 0) { disconnect(); throw std::runtime_error("db: send failed — connection lost"); }
        sent += r;
    }
#endif
}

std::vector<uint8_t> MySQLClient::read_packet(uint8_t& seq) {
    // Multi-packet continuation (0xFFFFFF): a payload over 16 MB is split by the server into EXACTLY-0xFFFFFF
    // packets; the receiver MUST keep reading and JOIN until a packet whose length is LESS than 0xFFFFFF
    // arrives. Without this logic read_packet would take the first 16 MB for a complete packet and return,
    // the next call would read the continuation bytes of the same payload as a HEADER -> the stream desynchronizes ->
    // the pooled connection silently returns WRONG DATA (triggered by LONGBLOB / wide SELECT * / large
    // GROUP_CONCAT). A normal <0xFFFFFF packet: goes around the loop once, break
    // -> behavior is the same as the earlier single-packet form.
    // Total cap: with multi-packet the 24-bit field is no longer an upper bound -> an explicit cap like the WS discipline
    // (otherwise a malicious server would exhaust memory with endless 0xFFFFFF continuations).
    // The joining logic is in look/mysql_wire_parse.h (pure, table-testable). The reader here
    // only provides the socket recv; the cap + 0xFFFFFF continuation decision is in one place, in the header.
    return look::mysql_reassemble(
        [this](uint8_t* buf, size_t n) { return recv_bytes(buf, n); }, seq);
}

void MySQLClient::send_packet(const std::vector<uint8_t>& data, uint8_t seq) {
    uint32_t len = (uint32_t)data.size();
    uint8_t hdr[4] = {(uint8_t)len,(uint8_t)(len>>8),(uint8_t)(len>>16),seq};
    // A small packet goes out with its header in a single send (previously two: each a separate system call + a separate
    // wake-up of the server). A large packet (BLOB) is not copied — two sends. The buffer is 4 KB: a
    // larger frame on a fiber stack touches pages for every fiber.
    if (data.size() <= 4092) {
        uint8_t one[4096];
        std::memcpy(one, hdr, 4);
        if (!data.empty()) std::memcpy(one + 4, data.data(), data.size());
        send_bytes(one, 4 + data.size());
        return;
    }
    send_bytes(hdr, 4);
    send_bytes(data.data(), data.size());
}

// ── MySQL authentication ───────────────────────────────────────────────────────

// ── caching_sha2_password (the MySQL 8.0+ DEFAULT plugin) ──────────────────
//
// OLD STATE: only `mysql_native_password` was implemented and the plugin name was
// WRITTEN FIXED in the handshake response. The plugin announced by the server was not read and
// AuthSwitchRequest (0xFE) was not handled at all. Result:
//   MySQL 5.7 / MariaDB  -> works
//   MySQL 8.0 (DEFAULT caching_sha2_password since 2018) -> CANNOT CONNECT
//   MySQL 8.4 (native plugin disabled) / 9.x (removed)       -> CANNOT CONNECT
// That is, the language effectively spoke MySQL 5.7.
//
// Fast path:  XOR( SHA256(password), SHA256( SHA256(SHA256(password)) + nonce ) )
// Full path:  the server asks for 0x04 -> without TLS the RSA public key is requested from the server
//             (0x02) and (password+NUL) XOR nonce is encrypted with RSA-OAEP.
// On a fresh server the cache is empty, so the FIRST connection always takes the full path.
//
// The crypto comes from OpenSSL — the code base has 8 separate copies of SHA-256
// (the S2 cluster); we do not add a ninth.
#ifndef _WIN32
// OpenSSL 3.0 deprecated the low-level SHA256_* API; EVP single-shot
// SHA-256 helper — one chains d2+nonce, the other is single-block.
static void evp_sha256(const unsigned char* a, size_t alen,
                       const unsigned char* b, size_t blen,
                       unsigned char out[SHA256_DIGEST_LENGTH]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("db mysql: SHA-256 context allocation failed");
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, a, alen) == 1 &&
              (blen == 0 || EVP_DigestUpdate(ctx, b, blen) == 1) &&
              EVP_DigestFinal_ex(ctx, out, nullptr) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("db mysql: SHA-256 digest failed");
}

static std::vector<uint8_t> caching_sha2_scramble(const std::string& password,
                                                  const std::string& nonce) {
    if (password.empty()) return {};
    unsigned char d1[SHA256_DIGEST_LENGTH], d2[SHA256_DIGEST_LENGTH], d3[SHA256_DIGEST_LENGTH];
    evp_sha256(reinterpret_cast<const unsigned char*>(password.data()), password.size(), nullptr, 0, d1);
    evp_sha256(d1, sizeof(d1), nullptr, 0, d2);
    evp_sha256(d2, sizeof(d2),
               reinterpret_cast<const unsigned char*>(nonce.data()), nonce.size(), d3);
    std::vector<uint8_t> out(SHA256_DIGEST_LENGTH);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) out[i] = d1[i] ^ d3[i];
    return out;
}

// (password + NUL) XOR nonce -> encrypt with OAEP using the server's RSA public key.
static std::vector<uint8_t> caching_sha2_rsa_encrypt(const std::string& password,
                                                     const std::string& nonce,
                                                     const std::string& pem) {
    std::string buf = password;
    buf.push_back('\0');
    if (!nonce.empty())
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = (char)(buf[i] ^ nonce[i % nonce.size()]);

    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) throw std::runtime_error("db mysql: could not read RSA key (BIO)");
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) throw std::runtime_error("db mysql: could not parse the server's RSA public key");

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
    if (!ok) throw std::runtime_error("db mysql: RSA encryption failed");
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
    if (pkt.empty()) throw std::runtime_error("db: empty handshake packet");
    const uint8_t* p = pkt.data();
    const uint8_t* end = p + pkt.size();

    if (*p == 0xFF) { p++; uint16_t e=(p+2<=end)?read_u16(p):0; throw std::runtime_error("db: server error "+std::to_string(e)); }

    p++; // protocol version
    while (p < end && *p) p++; if (p < end) p++; // server version (null-terminated)
    // Bounds-safe: a short/malicious handshake packet could overread past the buffer with unchecked reads
    // (challenge 8 bytes, auth_len, cap flags)
    // (MITM/malicious server — no TLS). Fixed block = thread(4)+challenge(8)+filler(1)+
    // cap1(2)+charset+status(3)+cap2(2)+auth_len(1)+reserved(10) = 31 bytes.
    if (end - p < 31) throw std::runtime_error("db: malformed handshake packet (too short)");
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

    // The authentication plugin announced by the server (comes with CLIENT_PLUGIN_AUTH).
    // OLD BUG: this field was NOT READ at all; the answer was always sent as mysql_native_password.
    // Since MySQL 8's default is caching_sha2_password
    // the connection could not be established.
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
                "db mysql: caching_sha2_password is not supported in this build (the Windows build "
                "has no OpenSSL). Either create the user with mysql_native_password, "
                "or use the Linux build.");
#endif
        }
        throw std::runtime_error("db mysql: unsupported authentication plugin: " + plugin);
    };

    std::string cur_plugin = srv_plugin;
    // CLIENT_MULTI_STATEMENTS (0x10000) and CLIENT_LOCAL_FILES (0x80) are NOT REQUESTED: user SQL
    // goes only as a prepared statement (it can use neither); the text query is only the fixed
    // BEGIN/COMMIT/SAVEPOINT. With the flag on, an "a; b" text query would leave the second result on the connection,
    // and a malicious server could ask for a file with a LOCAL INFILE request.
    uint32_t client_flags = 0x000EA605;
    if (!database.empty()) client_flags |= 0x00000008;

    uint8_t auth_seq = 1;
    if (cfg_.tls) {
#ifndef _WIN32
        // ── MySQL SSLRequest -> TLS upgrade (ORDER IS CRITICAL) ─────────────────────────
        // Order: initial handshake (read above, plaintext) -> SSLRequest (CLIENT_SSL
        // flag, NO auth data, seq 1) -> SSL_connect (upgrade) -> auth now over TLS
        // (seq 2). Auth is sent STRICTLY AFTER SSL_connect (once ssl_ is set
        // send_bytes switches to SSL_write) — otherwise credentials would leak in plaintext.
        client_flags |= 0x00000800;  // CLIENT_SSL
        std::vector<uint8_t> ssl_req;
        ssl_req.push_back(client_flags&0xFF); ssl_req.push_back((client_flags>>8)&0xFF);
        ssl_req.push_back((client_flags>>16)&0xFF); ssl_req.push_back((client_flags>>24)&0xFF);
        ssl_req.insert(ssl_req.end(), {0xFF,0xFF,0xFF,0x00}); // max packet size
        ssl_req.push_back(45);                                // charset (utf8mb4)
        ssl_req.insert(ssl_req.end(), 23, 0);                 // reserved
        send_packet(ssl_req, 1);                              // plaintext — there is no ssl_ yet
        // If the read buffer has extra plaintext that arrived before TLS, reject: those bytes would be
        // read as if they were the first reply of the encrypted session (the STARTTLS command-injection class).
        // An honest server sends nothing while waiting for the client's TLS handshake.
        if (rbuf_len_ != rbuf_pos_) {
            disconnect();
            throw std::runtime_error("db mysql: the server sent data before TLS started — connection refused");
        }
        SSL_CTX* ctx = mysql_ssl_ctx();
        if (!ctx) throw std::runtime_error("db mysql: could not create SSL_CTX (TLS)");
        SSL* s = SSL_new(ctx);
        if (!s) throw std::runtime_error("db mysql: SSL_new failed (TLS)");
        SSL_set_fd(s, (int)sock_);
        // SNI (when connecting by name) + name verification in verify mode: IP -> IP SAN, name -> DNS name
        // (look/tls_peer.h, Go crypto/tls). SSL_VERIFY_PEER makes SSL_connect fail
        // on a bad/untrusted cert. If name verification cannot be set up (rare — memory/corrupt string) do not connect:
        // otherwise the chain is verified but the name is SILENTLY not checked ("valid CA + wrong host" MITM).
        if (cfg_.tls_verify) SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
        if (!look::tls_set_peer(s, cfg_.host, cfg_.tls_verify)) {
            SSL_free(s);
            throw std::runtime_error("db mysql: could not set up TLS hostname verification (verify)");
        }
        int hs = look::fiber_ssl_connect(s, (int)sock_, cfg_.connect_timeout_ms);
        if (hs != 1) {
            std::string why = hs == 0 ? std::string() : look::tls_failure_reason(s);   // look/tls_peer.h
            SSL_free(s);
            throw std::runtime_error(hs == 0 ? std::string("db mysql: TLS handshake timed out")
                                             : "db mysql: TLS handshake failed: " + why);
        }
        ssl_     = s;   // from here on send/recv_bytes go over SSL (including auth)
        auth_seq = 2;   // the auth packet is inside TLS, with the seq after SSLRequest
#else
        throw std::runtime_error("db mysql: TLS (mysqls://) is not supported in this build "
                                 "(the Windows build is compiled without OpenSSL). Use the Linux build.");
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

    // Authentication dialog. OLD BUG: only 0xFF (error) was checked,
    // 0xFE (AuthSwitchRequest) and 0x01 (AuthMoreData) were NOT handled at all — yet
    // since the CLIENT_PLUGIN_AUTH flag is set the server was
    // entitled to send them. MySQL 8 does exactly this and the dialog broke.
    auto auth_error = [&](const std::vector<uint8_t>& r) {
        std::string msg = "db: authentication failed";
        if (r.size() > 3) {
            size_t off = 3;
            if (r[off] == '#') off += 6;
            if (off > r.size()) off = r.size();   // short error packet: do not go out of range (R24)
            msg = "db: " + std::string(r.begin() + off, r.end());
        }
        throw std::runtime_error(msg);
    };

    for (int round = 0; round < 5; ++round) {   // ceiling against an endless dialog
        auto resp = read_packet(seq);
        if (resp.empty()) throw std::runtime_error("db: empty authentication response");
        if (resp[0] == 0xFF) auth_error(resp);
        if (resp[0] == 0x00) return;            // OK — authenticated

        if (resp[0] == 0xFE) {
            // AuthSwitchRequest: [0xFE][plugin name NUL][new nonce]
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
            // AuthMoreData — caching_sha2_password flow
            uint8_t code = resp.size() > 1 ? resp[1] : 0;
            if (code == 0x03) continue;         // fast auth succeeded -> the next packet is OK
            if (code == 0x04) {
                // Full authentication (cache empty -> first connection).
#ifndef _WIN32
                if (ssl_) {
                    // TLS ACTIVE -> the channel is already encrypted; the password is sent in CLEARTEXT (NUL-terminated).
                    // The RSA public-key dance is NOT done (the server gives no RSA over TLS -> the old code
                    // would have crashed with "did not provide an RSA public key"). The MySQL protocol requires exactly this.
                    std::vector<uint8_t> cleartext(password.begin(), password.end());
                    cleartext.push_back(0x00);
                    send_packet(cleartext, (uint8_t)(seq + 1));
                    continue;
                }
#endif
                // No TLS -> the RSA public key is requested from the server (0x02), the password goes RSA-OAEP encrypted.
#ifdef _WIN32
                throw std::runtime_error(
                    "db mysql: caching_sha2_password full authentication is not supported in this build "
                    "not supported (the Windows build is compiled without OpenSSL). Fix: "
                    "create the user with mysql_native_password, or use the Linux build.");
#else
                send_packet(std::vector<uint8_t>{0x02}, (uint8_t)(seq + 1));
                auto keypkt = read_packet(seq);
                if (keypkt.size() < 2 || keypkt[0] != 0x01)
                    throw std::runtime_error("db mysql: server did not provide an RSA public key");
                std::string pem(keypkt.begin() + 1, keypkt.end());
                auto enc = caching_sha2_rsa_encrypt(password, challenge, pem);
                send_packet(enc, (uint8_t)(seq + 1));
                continue;
#endif
            }
            throw std::runtime_error("db mysql: unexpected authentication data (0x" +
                                     std::to_string((int)code) + ")");
        }
        throw std::runtime_error("db mysql: unexpected authentication packet");
    }
    throw std::runtime_error("db mysql: authentication did not complete (round limit exceeded)");
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
        // BOUNDS: if a malicious/MITM server sends a short error packet ({0xFF} or truncated)
        // read_u16 (unguarded p[0]|p[1]) READS past the buffer (OOB) — the handshake path
        // (:505) was protected but the query path was not. p+=6 and std::string(p,end)
        // can also overrun. Bound all of them.
        p++;
        uint16_t code = (p + 2 <= end) ? read_u16(p) : 0;
        if (p < end && *p == '#') { p += 6; if (p > end) p = end; }
        throw std::runtime_error("db: query error " + std::to_string(code) + ": " +
                                 std::string(p <= end ? p : end, end));
    }
    if (resp[0] == 0x00) {
        p++; affected_rows_=read_lenenc(p,end); last_insert_id_=read_lenenc(p,end);
        uint16_t st = (end - p >= 2) ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
        in_txn_ = (st & 0x01) != 0;   // SERVER_STATUS_IN_TRANS
        drain_more_results(st, seq);
        return {};
    }

    uint64_t col_count = read_lenenc(p, end);
    // Server-supplied col_count sanity cap — if a malicious server gives 2^64
    // the column-read loop exhausts resources/hangs. Real queries are far below 65535 columns.
    if (col_count > MYSQL_MAX_COLUMNS)
        throw std::runtime_error("db mysql: column count limit exceeded (malicious server?)");
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
    uint16_t st = 0;
    while (true) {
        auto row_pkt = read_packet(seq);
        // Error between rows (query cut off midway: timeout, KILL, conversion error) ->
        // previously the loop ended silently and an INCOMPLETE result was returned as if complete.
        if (!row_pkt.empty() && row_pkt[0] == 0xFF) throw_err_packet(row_pkt, "db: query error: ");
        if (row_pkt.empty() || (row_pkt[0] == 0xFE && row_pkt.size() < 9)) {
            if (row_pkt.size() >= 5) {   // EOF: [FE][warnings 2][status 2]
                st = (uint16_t)(row_pkt[3] | (row_pkt[4] << 8));
                in_txn_ = (st & 0x01) != 0;
            }
            break;
        }
        DbRow row;
        const uint8_t* rp = row_pkt.data();
        const uint8_t* re = rp + row_pkt.size();
        for (const auto& col : columns) {
            DbValue dv;
            if (rp < re && *rp == 0xFB) {
                rp++;             // consume the NULL marker
                dv.is_null = true;
            } else {
                dv.str  = read_lenenc_str(rp, re);
                dv.type = col.type;
            }
            row.push_back({col.name, std::move(dv)});
        }
        rows.push_back(std::move(row));
    }
    drain_more_results(st, seq);
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
        // Bounds-safe: on a short 0xFF packet (just FF) memcpy would overread 2 bytes
        // and move p past end -> std::string(p,end) UB. Triggered by a malicious/
        // MITM server. Skip error-code (2) and '#SQLSTATE' (6) with bounds checks.
        last_error_code_ = 0;
        if (p + 2 <= end) { last_error_code_ = (uint16_t)(p[0] | (p[1] << 8)); p += 2; } else p = end;   // error code (1243/1461 distinction)
        if (p < end && *p == '#' && p + 6 <= end) p += 6;   // '#SQLSTATE' marker
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
    // max_allowed_packet protection: default 64 MB
    static constexpr size_t MYSQL_MAX_PACKET = 64 * 1024 * 1024;
    size_t total_str = 0;
    for (const auto& p : params)
        if (p.kind == DbParam::TEXT_VAL) total_str += p.s.size();
    if (total_str > MYSQL_MAX_PACKET)
        throw std::runtime_error("db mysql: total parameter size exceeds the max_allowed_packet limit (64 MB)");

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
            if (params[i].kind == DbParam::BOOL_VAL)  tc = 0x01; // MYSQL_TYPE_TINY (0x10=BIT was WRONG; BIT expects lenenc, the value was being sent as 1 byte -> "Incorrect arguments")
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
        // Bounds-safe (same as the stmt_prepare error path): prevent the
        // 2-byte memcpy overread + std::string(p,end) UB on a short 0xFF packet.
        last_error_code_ = 0;
        if (p + 2 <= end) { last_error_code_ = (uint16_t)(p[0] | (p[1] << 8)); p += 2; } else p = end;   // error code (1243/1461 distinction)
        if (p < end && *p == '#' && p + 6 <= end) p += 6;   // '#SQLSTATE'
        throw std::runtime_error("db: " + std::string(p, end));
    }
    if (resp[0] == 0x00) { // OK (INSERT/UPDATE/DELETE)
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        affected_rows_  = read_lenenc(p, end);
        last_insert_id_ = read_lenenc(p, end);
        uint16_t st = (end - p >= 2) ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
        in_txn_ = (st & 0x01) != 0;   // SERVER_STATUS_IN_TRANS
        drain_more_results(st, seq);
        return {};
    }

    // Binary resultset
    const uint8_t* p = resp.data();
    const uint8_t* end = p + resp.size();
    uint64_t col_count = read_lenenc(p, end);
    // Sanity cap — if a malicious server gives a 2^64 col_count the loop + the `(int)col_count`
    // null-bitmap size (line ~657) overflows/exhausts resources.
    if (col_count > MYSQL_MAX_COLUMNS)
        throw std::runtime_error("db mysql: column count limit exceeded (malicious server?)");

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
    uint16_t st = 0;
    while (true) {
        auto row_pkt = read_packet(seq);
        // Error between rows -> an incomplete result would silently be returned (see query()).
        if (!row_pkt.empty() && row_pkt[0] == 0xFF) throw_err_packet(row_pkt, "db: ");
        if (row_pkt.empty() || row_pkt[0] == 0xFE) {   // a binary row starts with 0x00
            if (row_pkt.size() >= 5) {   // EOF: [FE][warnings 2][status 2]
                st = (uint16_t)(row_pkt[3] | (row_pkt[4] << 8));
                in_txn_ = (st & 0x01) != 0;
            }
            break;
        }

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
                // MySQL binary temporal decode. Previously DATE/DATETIME/TIMESTAMP/TIME
                // fell into `default`->read_lenenc_str -> raw wire bytes were returned
                // (DATETIME 7, DATE 4 bytes) -> the docs say "string YYYY-MM-DD...", VIOLATION.
                // Binary format: 1 length byte, then L data bytes. CRITICAL: each branch
                // must consume EXACTLY L bytes, otherwise the cursor drifts and the NEXT column is corrupted
                // (silent corruption) — so at the end rp = p0 + L aligns it exactly.
                auto read_temporal = [&](bool is_time) -> std::string {
                    int L = (int)read_le(1);                 // length byte (rp advanced by one)
                    const uint8_t* p0 = rp;                  // start of data
                    auto g = [&](int off)->int { return (p0+off < re) ? p0[off] : 0; };
                    char buf[48]; buf[0]=0;
                    if (is_time) {
                        if (L == 0) { std::strcpy(buf, "00:00:00"); }
                        else {
                            int sign = g(0);
                            uint32_t days = (uint32_t)(g(1) | (g(2)<<8) | (g(3)<<16) | ((uint32_t)g(4)<<24));
                            long th = (long)days*24 + g(5);
                            if (L >= 12) {
                                uint32_t us = (uint32_t)(g(8) | (g(9)<<8) | (g(10)<<16) | ((uint32_t)g(11)<<24));
                                std::snprintf(buf,sizeof buf,"%s%ld:%02d:%02d.%06u", sign?"-":"", th, g(6), g(7), us);
                            } else {
                                std::snprintf(buf,sizeof buf,"%s%ld:%02d:%02d", sign?"-":"", th, g(6), g(7));
                            }
                        }
                    } else {
                        if (L == 0) { std::strcpy(buf, "0000-00-00"); }
                        else {
                            int year = g(0) | (g(1)<<8);
                            if (L >= 7) {
                                if (L >= 11) {
                                    uint32_t us = (uint32_t)(g(7) | (g(8)<<8) | (g(9)<<16) | ((uint32_t)g(10)<<24));
                                    std::snprintf(buf,sizeof buf,"%04d-%02d-%02d %02d:%02d:%02d.%06u",year,g(2),g(3),g(4),g(5),g(6),us);
                                } else {
                                    std::snprintf(buf,sizeof buf,"%04d-%02d-%02d %02d:%02d:%02d",year,g(2),g(3),g(4),g(5),g(6));
                                }
                            } else {
                                std::snprintf(buf,sizeof buf,"%04d-%02d-%02d",year,g(2),g(3));
                            }
                        }
                    }
                    rp = p0 + L; if (rp > re) rp = re;       // EXACTLY L bytes: cursor alignment guaranteed
                    return std::string(buf);
                };
                switch(ct) {
                    case 0x01:           dv.str = std::to_string(read_le(1)); break;
                    case 0x02: case 0x0D: dv.str = std::to_string(read_le(2)); break;
                    case 0x03: case 0x09: dv.str = std::to_string(read_le(4)); break;
                    case 0x08:            dv.str = std::to_string(read_le(8)); break;
                    // The language's single double format (look/format.h) — NaN/Inf + short round-trip, the SAME text
                    // in every driver. (The old to_string=%f lost precision beyond 6 digits.)
                    // Truncated packet: the value is not returned as uninitialized stack memory (R24)
                    case 0x04: { float f = 0;  if(rp+4<=re){memcpy(&f,rp,4);rp+=4;} else rp=re; dv.str=look_format_double(f); break; }
                    case 0x05: { double d = 0; if(rp+8<=re){memcpy(&d,rp,8);rp+=8;} else rp=re; dv.str=look_format_double(d); break; }
                    case 0x10:            dv.str = std::to_string(read_le(1)); break;
                    // Temporal: DATE(0x0A)/NEWDATE(0x0E)/TIMESTAMP(0x07)/DATETIME(0x0C) -> date,
                    // TIME(0x0B) -> time. The length L (0/4/7/11, TIME 0/8/12) determines the format.
                    case 0x0A: case 0x0E:
                    case 0x07: case 0x0C: dv.str = read_temporal(false); break;
                    case 0x0B:            dv.str = read_temporal(true);  break;
                    default:              dv.str = read_lenenc_str(rp, re); break;
                }
            }
            row.push_back({columns[ci].name, std::move(dv)});
        }
        rows.push_back(std::move(row));
    }
    drain_more_results(st, seq);
    return rows;
}

std::vector<DbRow> MySQLClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    ensure_connected();
    // USE is rejected in the prepared-statement protocol today; if it were supported the database would change and
    // cached statements would stay bound to the old database's tables -> the cache is cleared.
    if (sql.size() >= 4 && (sql[0] | 0x20) == 'u' && (sql[1] | 0x20) == 's' && (sql[2] | 0x20) == 'e' &&
        (sql[3] == ' ' || sql[3] == '\t' || sql[3] == '\n'))
        clear_stmt_cache(true);

    last_error_code_ = 0;   // so that the code of an earlier error does not mislead the distinction below
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) {
        it->second.used = ++stmt_tick_;
        try {
            return stmt_execute(it->second.meta, params);
        } catch (const std::runtime_error&) {
            // 1243: the statement does not exist on the server (a connection multiplexer — ProxySQL etc. — switched to another
            // backend). The query DID NOT RUN -> re-preparing and trying once is safe. Any other error
            // goes to the caller as is.
            if (last_error_code_ != 1243 || !is_connected()) throw;
            stmt_cache_.erase(it);
        }
    }

    StmtMeta meta;
    last_error_code_ = 0;
    try {
        meta = stmt_prepare(sql);
    } catch (const std::runtime_error&) {
        // 1461: the server's total statement limit (max_prepared_stmt_count, for all clients)
        // is full. Empty our cache and run this query without caching — the query does not fail.
        if (last_error_code_ != 1461 || !is_connected()) throw;
        clear_stmt_cache(true);
        auto m = stmt_prepare(sql);
        auto rows = stmt_execute(m, params);
        stmt_close(m.id);
        return rows;
    }
    if (stmt_cache_.size() >= STMT_CACHE_MAX) {   // close the least recently used one
        auto old = stmt_cache_.begin();
        for (auto i = stmt_cache_.begin(); i != stmt_cache_.end(); ++i)
            if (i->second.used < old->second.used) old = i;
        uint32_t old_id = old->second.meta.id;
        stmt_cache_.erase(old);   // Delete FIRST: if stmt_close fails, disconnect() empties the map
        stmt_close(old_id);
        if (!is_connected()) throw std::runtime_error("db: send failed — connection lost");
    }
    stmt_cache_.emplace(sql, CachedStmt{meta, ++stmt_tick_});
    try {
        return stmt_execute(meta, params);
    } catch (const std::runtime_error&) {
        // Mirror of the cache path: if the multiplexer switched backend between prepare and execute
        // (1243) the query DID NOT RUN — re-prepare and try once. Any other error goes to the caller.
        if (last_error_code_ != 1243 || !is_connected()) throw;
        stmt_cache_.erase(sql);
        last_error_code_ = 0;
        StmtMeta again = stmt_prepare(sql);
        stmt_cache_.emplace(sql, CachedStmt{again, ++stmt_tick_});
        return stmt_execute(again, params);
    }
}

// ── Escape ────────────────────────────────────────────────────────────────────

// Quote escape is `''` (STANDARD SQL) — NOT `\'`.
//
// OLD BUG: `'` was written as `\'`. In MySQL `NO_BACKSLASH_ESCAPES` mode the backslash
// is NOT treated as an escape character; in that mode `\'` means "backslash + quote" ->
// THE QUOTE CLOSES -> SQL injection. MEASURED (real server, with the session put
// into that mode): the payload `' OR 1=1 -- ` returned the **whole table** (3/3 rows).
//
// The only thing holding this was the SUCCESS of the `SET SESSION sql_mode = REPLACE(...)`
// command in do_connect(). That is not a sufficient security floor:
//   * ProxySQL / connection multiplexers multiplex the backend connections —
//     the SET succeeds, then the session state can be silently lost; the connection
//     is not broken, only the protection disappears.
//   * The rejection of a statement does not mean the connection is broken
//     (a proxy filter, an audit plugin, a governor can reject a single statement).
// The asymmetry decides: the fix is one line, the cost of being wrong is SQL injection.
//
// `''` is safe in BOTH modes (standard SQL; MySQL accepts it in normal mode too)
// -> security is no longer DEPENDENT on the success of `SET`. The PostgreSQL side
// already used `''`; this change brings the two drivers to the same footing.
//
// `SET SESSION` STAYS — but no longer for security, for DATA FIDELITY:
// in NBE mode the `\\` doubling below corrupts data (two backslashes are stored).
// That is a correctness matter, not a security one.
std::string MySQLClient::escape(const std::string& s) {
    std::string out; out.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\'': out += "''";   break;   // standard SQL — safe in NBE mode too
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

// These parsers were extracted to look/mysql_wire_parse.h (pure, the single definition shared with fuzzing).
// The members are one-line forwarders — call sites do not change, no drift.
uint64_t    MySQLClient::read_lenenc(const uint8_t*& p, const uint8_t* end) { return mysql_read_lenenc(p, end); }
std::string MySQLClient::read_lenenc_str(const uint8_t*& p, const uint8_t* end) { return mysql_read_lenenc_str(p, end); }
uint16_t    MySQLClient::read_u16(const uint8_t*& p) { return mysql_read_u16(p); }
uint32_t    MySQLClient::read_u32(const uint8_t*& p) { return mysql_read_u32(p); }
uint64_t    MySQLClient::read_u64(const uint8_t*& p) { return mysql_read_u64(p); }

} // namespace look

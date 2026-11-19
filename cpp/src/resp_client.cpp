#include "look/resp_client.h"
#include "look/db_dsn.h"   // redis_resolve_tls (pure TLS decision seam)
#include "look/host_port.h" // host[:port] + port parsing (Go net/url model)
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <stdexcept>
#include <vector>
#include <algorithm>

#ifndef _WIN32
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netdb.h>
#  include <unistd.h>
#  define CLOSESOCKET(fd) ::close(fd)
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define CLOSESOCKET(fd) ::closesocket(fd)
#endif

#ifndef _WIN32
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#include "look/tls_peer.h"   // SNI + name verification (IP → IP SAN), handshake failure reason
#endif
#include "look/sock_idle.h"   // idle_socket_unusable: liveness probe of a connection taken from the pool
#include "look/fiber_net.h"   // while waiting on Redis inside a fiber (DNS/connect/TLS/write/read) the thread is released

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0   // Windows/macOS: no such flag (Windows has no SIGPIPE either)
#endif

namespace look {

#ifndef _WIN32
// ── Redis TLS: client SSL_CTX (same pattern as mysql_client) ─────────────────
// rediss:// / ?tls=1 → encryption (VERIFY_NONE, self-signed friendly); ?tls=verify →
// SSL_VERIFY_PEER + hostname (against MITM). Managed Redis (Upstash/ElastiCache/Redis
// Cloud) REQUIRES TLS — the old stub-throw blocked all of them.
static SSL_CTX* redis_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        look::tls_client_defaults(c);   // at least TLS 1.2, independent of the machine
        if (c) { SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr); SSL_CTX_set_default_verify_paths(c); }
        return c;
    }();
    return ctx;
}
#endif

// ── URL parser ───────────────────────────────────────────────────────────────
// redis://[:password@]host[:port][/db]
// rediss://...   (TLS — OpenSSL handshake after the socket is set up; stub for now)

static void parse_url(const std::string& url,
                      std::string& host, int& port,
                      std::string& pass, int& db, bool& tls, bool& tls_verify)
{
    host = "127.0.0.1";
    port = 6379;
    pass = "";
    db   = 0;
    tls  = false;
    tls_verify = false;

    std::string rest;
    // OLD BUG: substr(0,8)=="rediss://" — "rediss://" is 9 characters → an 8-character substr could NEVER
    // match (rediss:// never triggered TLS; it stayed hidden because it was a stub-throw).
    // rfind(...,0)==0 = the correct "starts with" check.
    if (url.rfind("rediss://", 0) == 0) { tls = true; rest = url.substr(9); }
    else if (url.rfind("redis://", 0) == 0)             rest = url.substr(8);
    else                                                 rest = url;

    // ?tls=verify / ?ssl=verify → encryption + certificate/hostname verification (against MITM).
    // ?tls=1 → encryption only. Extract the query string from rest.
    // The split is in look/db_dsn.h (pure, table-testable): last '@' first, then '?'.
    // '?' used to be searched first → a '?' in the password gave a wrong host and query.
    { std::string userinfo, hostpart, query;
      look::redis_split_url(rest, userinfo, hostpart, query);
      look::redis_resolve_tls(query, tls, tls, tls_verify);
      // password: [user]:pass
      auto colon = userinfo.find(':');
      if (colon != std::string::npos) pass = userinfo.substr(colon + 1);
      rest = hostpart; }

    // /db
    // Go-redis ParseURL model: a malformed database number or port is an ERROR. Both used to silently
    // fall back to the default ("/abc" → db 0: writing to another application's keys; ":63799" accepted).
    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        const std::string ds = rest.substr(slash + 1);
        if (!ds.empty()) {
            if (ds.size() > 9 || ds.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Redis: invalid database number \"" + ds + "\" in the URL");
            db = std::stoi(ds);
        }
        rest = rest.substr(0, slash);
    }

    // host:port — IPv6 in square brackets ("[::1]:6379"); with the old rfind(':') "[::1]" gave host "[:"
    std::string err = look::split_host_port(rest, host, port);
    if (!err.empty()) throw std::runtime_error("Redis: invalid URL: " + err);
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

// Time limit for the Redis reply (and connecting, handshake, sending). There used to be no limit:
// a Redis that accepted but never answered hung the request forever. LOOK_REDIS_TIMEOUT_MS (ms).
static int redis_timeout_ms() {
    static const int v = [] {
        const char* e = std::getenv("LOOK_REDIS_TIMEOUT_MS");
        if (e && *e) { long n = std::atol(e); if (n > 0 && n <= 3600000) return (int)n; }
        return 3000;
    }();
    return v;
}

RespClient::RespClient(const std::string& url) {
    tmo_ms_ = redis_timeout_ms();
    parse_url(url, host_, port_, pass_, db_, tls_, tls_verify_);
    // LOOK_DB_REQUIRE_TLS — reject a plaintext connection to a remote host (loopback is allowed,
    // see db_dsn.h db_refuse_plaintext). EXTRA critical for Redis: AUTH sends the password in cleartext
    // (no challenge-response) → plaintext remote Redis = the password is exposed on the wire.
    {
        const char* e = std::getenv("LOOK_DB_REQUIRE_TLS");
        bool require_tls = e && *e && std::string(e) != "0" && std::string(e) != "false";
        if (look::db_refuse_plaintext(require_tls, tls_, host_,
                                      look::db_insecure_plaintext_opt(url)))
            throw std::runtime_error(
                "redis: refusing plaintext connection to remote host '" + host_ +
                "' (LOOK_DB_REQUIRE_TLS is set; plaintext Redis AUTH would expose the password). "
                "Use rediss:// (encrypted+verified), or add ?insecure_plaintext=1 to the URL, "
                "or unset LOOK_DB_REQUIRE_TLS.");
    }
    connect();
}

RespClient::~RespClient() { drop(); }

// Close the connection. After a command left halfway it is unknown where in the stream we are
// (a reply that has not arrived comes later and is taken for the reply to the next command) → on every error and
// timeout the connection is dropped; it is never used again (is_connected false).
void RespClient::drop() {
#ifndef _WIN32
    if (ssl_) { SSL_free((SSL*)ssl_); ssl_ = nullptr; }
#endif
    if (fd_ >= 0) { CLOSESOCKET(fd_); fd_ = -1; }
    rpos_ = rlen_ = 0;   // unread data left from the old connection must not carry over to the new one
}

// Is a connection waiting in the pool still usable — WITHOUT SENDING ANYTHING (same probe as
// MySQL/PG). If Redis restarted or closed it on timeout the socket becomes readable (EOF);
// previously the first command went to the dead socket and the session request got a 503.
bool RespClient::usable_when_idle() {
    bool ok = fd_ >= 0 && rpos_ == rlen_;
#ifndef _WIN32
    if (ok && ssl_ && SSL_pending((SSL*)ssl_) > 0) ok = false;
#endif
    if (ok && idle_socket_unusable(fd_)) ok = false;
    if (!ok) drop();
    return ok;
}

void RespClient::fail(const std::string& msg) {
    drop();
    throw std::runtime_error(msg);
}

void RespClient::fail_timeout(const char* what) {
    fail(std::string("Redis: ") + what + " " + host_ + ":" + std::to_string(port_) + " took longer than " +
         std::to_string(tmo_ms_) + " ms (LOOK_REDIS_TIMEOUT_MS); the connection was closed");
}

// ── TCP connect ──────────────────────────────────────────────────────────────

void RespClient::connect() {
    drop();

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (fiber_getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0)
        throw std::runtime_error("Redis: cannot resolve " + host_);

    int sock = -1;
    bool timed_out = false;
    for (auto* r = res; r; r = r->ai_next) {
        sock = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (sock < 0) continue;
#ifndef _WIN32
        // Reads/writes are time-limited outside a fiber too (waits inside a fiber use tmo_ms_).
        struct timeval tv; tv.tv_sec = tmo_ms_ / 1000; tv.tv_usec = (tmo_ms_ % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (fiber_connect(sock, r->ai_addr, (socklen_t)r->ai_addrlen, tmo_ms_) == 0) break;
        if (errno == ETIMEDOUT) timed_out = true;
#else
        DWORD tv = (DWORD)tmo_ms_;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        if (::connect(sock, r->ai_addr, (int)r->ai_addrlen) == 0) break;
#endif
        CLOSESOCKET(sock); sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0 && timed_out) fail_timeout("connecting to");
    if (sock < 0) throw std::runtime_error("Redis: cannot connect to " + host_ + ":" + std::to_string(port_));
    fd_ = sock;

    // Turn off Nagle's algorithm — avoids a 40ms delay on small packets
    {
        int nd = 1;
#ifdef _WIN32
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
#else
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
#endif
    }

    if (tls_) {
#ifndef _WIN32
        // TLS: SSL handshake right after TCP is established (unlike MySQL there is no mid-stream
        // SSLRequest — Redis TLS is at the start of the connection). Once ssl_ is set all I/O goes through SSL.
        SSL_CTX* ctx = redis_ssl_ctx();
        if (!ctx) throw std::runtime_error("Redis: could not create SSL_CTX (TLS)");
        SSL* s = SSL_new(ctx);
        if (!s) throw std::runtime_error("Redis: SSL_new failed (TLS)");
        SSL_set_fd(s, fd_);
        // SNI + name verification (IP → IP SAN, name → DNS name; look/tls_peer.h). If it cannot be set up we do not connect,
        // so name checking does not silently turn off (see mysql_client).
        if (tls_verify_) SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
        if (!tls_set_peer(s, host_, tls_verify_)) {
            SSL_free(s);
            fail("Redis: could not set up TLS hostname verification (verify)");
        }
        int hs = fiber_ssl_connect(s, fd_, tmo_ms_);
        if (hs == 0) { SSL_free(s); fail_timeout("the TLS handshake with"); }
        if (hs != 1) {
            std::string why = tls_failure_reason(s);   // look/tls_peer.h
            SSL_free(s);
            fail("Redis: TLS handshake failed: " + why);
        }
        ssl_ = s;   // from now on send_command/read_* go over SSL
#else
        throw std::runtime_error("Redis TLS (rediss://) is not supported in this build "
                                 "(the Windows build is compiled without OpenSSL). Use the Linux build.");
#endif
    }

    // AUTH
    if (!pass_.empty()) {
        send_command({"AUTH", pass_});
        auto r = read_response();
        if (r != "OK") throw std::runtime_error("Redis AUTH failed");
    }
    // SELECT db
    if (db_ != 0) {
        send_command({"SELECT", std::to_string(db_)});
        read_response();
    }
}

// ── RESP2 writer ─────────────────────────────────────────────────────────────

void RespClient::send_command(const std::vector<std::string>& args) {
    std::string buf;
    buf += "*" + std::to_string(args.size()) + "\r\n";
    for (auto& a : args)
        buf += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";

    if (fd_ < 0) throw std::runtime_error("Redis: not connected");
#ifndef _WIN32
    if (!fiber_send_all(fd_, (SSL*)ssl_, buf.data(), buf.size(), tmo_ms_)) {
        if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) fail_timeout("sending to");
        fail("Redis: send error");
    }
#else
    const char* p = buf.data();
    size_t rem = buf.size();
    while (rem > 0) {
        int sent = (int)::send(fd_, p, (int)rem, 0);
        if (sent <= 0) fail("Redis: send error");
        p += sent; rem -= sent;
    }
#endif
}

// ── RESP2 reader ─────────────────────────────────────────────────────────────

// Single read point: reads at most 16 KB into the buffer. Previously recv/SSL_read blocked directly
// (inside a fiber the THREAD could not serve another request until the Redis reply arrived) and lines were read byte
// by byte (every byte a system call). If data is ready it is read at once; otherwise only the fiber
// waits at most tmo_ms_. Outside a fiber the read blocks, bounded by SO_RCVTIMEO (tmo_ms_).
// If the time runs out or the connection drops, the connection is dropped and a clear error is thrown.
void RespClient::fill() {
    if (fd_ < 0) throw std::runtime_error("Redis: not connected");
    rbuf_.resize(16384);
    rpos_ = rlen_ = 0;
    int n;
#ifndef _WIN32
    if (!(ssl_ && SSL_pending((SSL*)ssl_) > 0) && fiber_wait_readable(fd_, tmo_ms_) == 0)
        fail_timeout("waiting for a reply from");
    errno = 0;
    // Waited above inside the fiber (data is ready); outside a fiber → blocking-ok: bounded by SO_RCVTIMEO.
    if (ssl_) n = SSL_read((SSL*)ssl_, &rbuf_[0], (int)rbuf_.size());
    else      n = (int)::recv(fd_, &rbuf_[0], rbuf_.size(), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) fail_timeout("waiting for a reply from");
#else
    n = (int)::recv(fd_, &rbuf_[0], (int)rbuf_.size(), 0);
    if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) fail_timeout("waiting for a reply from");
#endif
    if (n <= 0) fail("Redis: connection closed");
    rlen_ = (size_t)n;
}

std::string RespClient::read_line() {
    // RESP2 control lines are short (+OK, :123, $<len>). An upper limit to prevent unbounded growth (OOM)
    // if a malicious/malformed server never sends '\n'.
    static constexpr size_t RESP_MAX_LINE = 64 * 1024;
    std::string line;
    while (true) {
        if (rpos_ >= rlen_) fill();
        char c = rbuf_[rpos_++];
        if (c == '\r') continue;
        if (c == '\n') break;
        line += c;
        if (line.size() > RESP_MAX_LINE)
            fail("Redis: line limit exceeded");
    }
    return line;
}

// Upper limit for a Redis bulk reply — so a malicious/malformed server does not return $2000000000
// and trigger an unbounded allocation (OOM DoS). Set with LOOK_REDIS_MAX_BULK
// (bytes), default 64 MB.
static size_t resp_max_bulk() {
    static const size_t v = []() -> size_t {
        const char* e = std::getenv("LOOK_REDIS_MAX_BULK");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return 64 * 1024 * 1024;
    }();
    return v;
}

std::string RespClient::read_bulk(long len) {
    if (len < 0) return "";  // null bulk
    // Bounds check BEFORE narrowing to int — a value > INT_MAX such as $5000000000
    // would wrap negative in (int)len, silently return null and desync the socket.
    if ((size_t)len > resp_max_bulk())
        fail("Redis: bulk response limit exceeded (LOOK_REDIS_MAX_BULK)");
    std::string buf((size_t)len, '\0');
    size_t got = 0, need = (size_t)len;
    while (got < need) {
        if (rpos_ >= rlen_) fill();
        size_t k = std::min(need - got, rlen_ - rpos_);
        std::memcpy(&buf[got], &rbuf_[rpos_], k);
        rpos_ += k; got += k;
    }
    read_line();  // trailing \r\n
    return buf;
}

std::string RespClient::read_response(int depth) {
    // A deeply nested array (*1\r\n*1\r\n...) can overflow the stack in the recursive parse —
    // depth limit against a malicious/malformed server.
    static constexpr int RESP_MAX_DEPTH = 64;
    if (depth > RESP_MAX_DEPTH)
        fail("Redis: response nested too deep (array)");
    auto line = read_line();
    if (line.empty()) fail("Redis: empty response");

    char type = line[0];
    std::string payload = line.substr(1);

    switch (type) {
        case '+': return payload;                        // Simple string
        case '-':                                        // Error
            // At top level the stream is aligned (reply finished) → the connection stays intact; inside an array
            // we exit without reading the remaining elements → the connection is dropped.
            if (depth > 0) fail("Redis: " + payload);
            throw std::runtime_error("Redis: " + payload);
        case ':': return payload;                        // Integer (as string)
        case '$': {                                      // Bulk string
            long len = 0;
            try { len = std::stol(payload); }            // malformed length → protocol error
            catch (...) { fail("Redis: invalid bulk length"); }
            if (len == -1) return "";                    // nil
            return read_bulk(len);
        }
        case '*': {                                      // Array — flatten for our use
            long count = 0;
            try { count = std::stol(payload); }
            catch (...) { fail("Redis: invalid array count"); }
            if (count <= 0) return "";
            // Server-supplied count sanity cap — if a malicious/MITM server (no TLS) gives
            // `*2000000000` the loop consumes resources. The session subset works with small
            // arrays; a reasonable upper limit. (Consistent with MySQL col_count /
            // RESP_MAX_DEPTH.)
            static constexpr long RESP_MAX_ARRAY = 1024 * 1024;
            if (count > RESP_MAX_ARRAY)
                fail("Redis: array element count limit exceeded");
            std::string first;
            for (int i = 0; i < count; i++) {
                auto v = read_response(depth + 1);
                if (i == 0) first = v;
            }
            return first;
        }
        default:
            fail("Redis: unknown response type " + std::string(1, type));
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

// SET ... XX: writes only if the key exists (single command, atomic). The reply is +OK or nil.
bool RespClient::set_if_exists(const std::string& key, const std::string& val, int ttl_sec) {
    if (ttl_sec > 0)
        send_command({"SET", key, val, "EX", std::to_string(ttl_sec), "XX"});
    else
        send_command({"SET", key, val, "XX"});
    return read_response() == "OK";
}

void RespClient::set(const std::string& key, const std::string& val, int ttl_sec) {
    if (ttl_sec > 0)
        send_command({"SET", key, val, "EX", std::to_string(ttl_sec)});
    else
        send_command({"SET", key, val});
    read_response();
}

std::string RespClient::get(const std::string& key) {
    send_command({"GET", key});
    return read_response();  // "" if nil
}

// GET, but "missing" (nil, $-1) is distinguished from "empty value" ($0): true = the key exists. One round trip, same as EXISTS.
bool RespClient::get_if_exists(const std::string& key, std::string& out) {
    out.clear();
    send_command({"GET", key});
    auto line = read_line();
    if (line.empty()) fail("Redis: empty response");
    if (line[0] == '-') throw std::runtime_error("Redis: " + line.substr(1));   // stream aligned
    if (line[0] != '$') fail("Redis: unexpected reply to GET");
    long len = 0;
    try { len = std::stol(line.substr(1)); } catch (...) { fail("Redis: invalid bulk length"); }
    if (len == -1) return false;
    out = read_bulk(len);
    return true;
}

void RespClient::del(const std::string& key) {
    send_command({"DEL", key});
    read_response();
}

bool RespClient::exists(const std::string& key) {
    send_command({"EXISTS", key});
    auto r = read_response();
    return r == "1";
}

void RespClient::expire(const std::string& key, int ttl_sec) {
    send_command({"EXPIRE", key, std::to_string(ttl_sec)});
    read_response();
}

} // namespace look

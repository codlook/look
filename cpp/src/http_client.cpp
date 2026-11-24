// http_client.cpp — self-contained HTTP/HTTPS client
// Linux: OpenSSL (system libssl)
// Windows: Schannel (built-in, WinSSL)

#include "look/http_client.h"
#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normally CMake compile-def provides it (single source)
#endif
#include "look/ssrf_safe.h"   // SSRF private/special-block decision (pure, table-testable)
#include "look/gzip_inflate.h"   // gzip response decode (pure, fuzzed)
#include "look/host_port.h" // host[:port] + port parsing (Go net/url model)
#include "look/url_parse.h"  // parse_url (tests/url_parse_referee_test.cpp checks it against Go)
#include "look/charset.h"     // single-byte charset → UTF-8 (iso-8859-9 etc.)
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>
#ifndef _WIN32
#  include <sys/stat.h>
#endif
#ifdef __linux__
#  include "look/fiber.h"   // fiber-aware recv (Go netpoller) — Linux --mode http only
#endif
#ifndef _WIN32
#  include "look/fiber_net.h"   // so that DNS/connect/TLS/send inside a fiber do not hold the thread
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define SECURITY_WIN32
#  include <security.h>
#  include <schannel.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "secur32.lib")
typedef SOCKET sock_t;
static const sock_t INVALID = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#include "look/tls_peer.h"   // SNI + name verification (IP → IP SAN), handshake failure reason
typedef int sock_t;
static const sock_t INVALID = -1;
#endif
#include <climits>

namespace look {

#ifdef _WIN32
// Send the WHOLE request (Windows; the POSIX path uses look::fiber_send_all). A single send() may write
// partially: when the socket's send time limit (the "timeout" option, SO_SNDTIMEO) expires on a slow-reading server,
// it sent what it could write and returned, and the rest never went — and no error was given either
// (measured: 2.6 MB of an 8 MB POST, error=null). Every send waits with the same time limit.
// Return: true = all of it went; false = error, *timed_out = time expired.
static bool send_all(sock_t s, const char* p, size_t n, bool* timed_out) {
    *timed_out = false;
    while (n > 0) {
        int chunk = n > (size_t)INT_MAX ? INT_MAX : (int)n;
        int k = send(s, p, chunk, 0);
        if (k == SOCKET_ERROR) { *timed_out = (WSAGetLastError() == WSAETIMEDOUT); return false; }
        if (k == 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
#endif

// ── Find the system CA bundle (for static OpenSSL — defined on both platforms) ──
// Static OpenSSL looks for the CA directory in the build prefix (ABSENT on the target) → https
// verification silently fails. We probe the standard paths and set SSL_CERT_FILE/DIR.
// If it is already set we do not touch it (OS/crypto-policies). Windows: Schannel uses the system
// cert store → no-op.
void configure_system_ca_bundle() {
#ifndef _WIN32
    if (!std::getenv("SSL_CERT_FILE")) {
        static const char* files[] = {
            "/etc/ssl/certs/ca-certificates.crt",   // Debian/Ubuntu/Alpine
            "/etc/pki/tls/certs/ca-bundle.crt",     // RHEL/AlmaLinux/Fedora/CentOS
            "/etc/ssl/ca-bundle.pem",               // openSUSE
            "/etc/pki/tls/cacert.pem",
            "/etc/ssl/cert.pem",                    // BSD/macOS/Alpine
        };
        for (const char* f : files) {
            struct stat st;
            if (::stat(f, &st) == 0 && S_ISREG(st.st_mode)) { ::setenv("SSL_CERT_FILE", f, 0); break; }
        }
    }
    if (!std::getenv("SSL_CERT_DIR")) {
        static const char* dirs[] = { "/etc/ssl/certs", "/etc/pki/tls/certs" };
        for (const char* d : dirs) {
            struct stat st;
            if (::stat(d, &st) == 0 && S_ISDIR(st.st_mode)) { ::setenv("SSL_CERT_DIR", d, 0); break; }
        }
    }
#endif
}

// ── SSRF protection — blocks connections to private/loopback IPs ─────────────────

// The decision logic is in look/ssrf_safe.h (pure, table-testable). The loop
// here only derives (family, raw bytes) from addrinfo and delegates to the predicate.
static bool is_ssrf_blocked(const struct addrinfo* res) {
    // can be disabled with LOOK_ALLOW_SSRF=1 (for an internal-network test environment)
    static const bool allow = (std::getenv("LOOK_ALLOW_SSRF") != nullptr &&
                               std::string(std::getenv("LOOK_ALLOW_SSRF")) == "1");
    if (allow) return false;

    for (const struct addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            uint32_t ip = ntohl(((struct sockaddr_in*)p->ai_addr)->sin_addr.s_addr);
            if (look::is_private_v4(ip)) return true;
        } else if (p->ai_family == AF_INET6) {
            const uint8_t* b = ((struct sockaddr_in6*)p->ai_addr)->sin6_addr.s6_addr;
            if (look::ssrf_blocked_v6(b)) return true;
        }
    }
    return false;
}

// The REASON for a connection failure — carried from tcp_connect to the caller.
// OLD BUG: tcp_connect returned the SAME INVALID for a DNS error, an SSRF block, a socket error and a real connection
// error; the caller called all of them "connection
// failed". Result: an SSRF BLOCK and a REAL NETWORK ERROR could not be told apart —
// a developer who could not reach an internal service could not see that a security policy
// blocked the request, and investigated network/DNS/firewall in vain.
// (Same class as bug 33: the behavior is right, the message does not allow diagnosis.)
// fiber-ok: written only AFTER tcp_connect's last suspension point, and the caller reads it immediately
static thread_local const char* t_conn_error = nullptr;

// ── URL parser: look/url_parse.h ─────────────────────────────────────────────
// ── Request builder ───────────────────────────────────────────────────────────

// CRLF stripping — prevents request-splitting / header injection in the outgoing request.
// If a user-controlled URL path, host or header value contains \r\n, it must not inject a fake
// header or request boundary (the server side does the same defense in the response
// header; the client applies the same discipline).
static std::string hc_strip_crlf(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) if (c != '\r' && c != '\n') r += c;
    return r;
}

// Host header / CONNECT target: IPv6 in square brackets ("[::1]:8080"); it used to be written without brackets
// ("Host: ::1:8080" — the server sees a broken Host). with_port=false → the default port is not written.
static std::string url_authority(const ParsedUrl& u, bool with_port) {
    std::string h = hc_strip_crlf(u.host);
    if (h.find(':') != std::string::npos) h = "[" + h + "]";
    if (with_port || (u.tls && u.port != 443) || (!u.tls && u.port != 80)) h += ":" + std::to_string(u.port);
    return h;
}

// Parse a proxy spec ("http://host:port", "host:port", "host", "[::1]:3128") into host + port.
// Go model (http.ProxyURL + canonicalAddr): 80 if there is no port; IPv6 in square brackets.
// "https://" proxy: LOOK does not connect to the proxy with TLS — the scheme used to be silently dropped and plain text
// spoken (a proxy expecting TLS breaks the connection). Now an explicit error.
static bool parse_proxy(const std::string& spec, std::string& host, int& port) {
    std::string s = spec;
    std::string low = s.substr(0, 8);
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.rfind("http://", 0) == 0)       s = s.substr(7);
    else if (low.rfind("https://", 0) == 0) return false;
    else if (s.find("://") != std::string::npos) return false;
    s = s.substr(0, s.find_first_of("/?#"));
    port = 80;
    host.clear();
    return look::split_host_port(s, host, port).empty() && !host.empty();
}

static std::string build_request(const std::string& method,
                                  const ParsedUrl& url,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& extra_headers,
                                  bool want_gzip = false,
                                  bool absolute_uri = false)  // proxy HTTP uses the absolute-form target
{
    std::ostringstream req;
    std::string target = hc_strip_crlf(url.path);
    if (absolute_uri)
        target = std::string(url.tls ? "https://" : "http://") + url_authority(url, false) + hc_strip_crlf(url.path);
    req << hc_strip_crlf(method) << " " << target << " HTTP/1.1\r\n";
    req << "Host: " << url_authority(url, false) << "\r\n";
    // It used to write a fixed "LOOK/0.19" (it was not updated with the version).
    req << "User-Agent: LOOK/" LOOK_VERSION "\r\n";
    // "user:pass@" in the URL → Basic credentials (Go): if the caller supplied its own Authorization, that wins.
    if (!url.basic_auth.empty()) {
        bool has_auth = false;
        for (auto& [k, v] : extra_headers) {
            std::string lk = k; for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
            if (lk == "authorization") { has_auth = true; break; }
        }
        if (!has_auth) req << "Authorization: Basic " << url.basic_auth << "\r\n";
    }
    req << "Connection: close\r\n";
    req << "Accept: */*\r\n";
    // Ask for gzip only when we can decode it (non-streaming) and the caller
    // didn't set its own Accept-Encoding.
    if (want_gzip && extra_headers.find("Accept-Encoding") == extra_headers.end())
        req << "Accept-Encoding: gzip\r\n";

    for (auto& [k, v] : extra_headers)
        req << hc_strip_crlf(k) << ": " << hc_strip_crlf(v) << "\r\n";

    if (!body.empty()) {
        req << "Content-Length: " << body.size() << "\r\n";
        // Content-Type already in extra_headers if user set it
        if (extra_headers.find("Content-Type") == extra_headers.end())
            req << "Content-Type: application/x-www-form-urlencoded\r\n";
    }
    req << "\r\n";
    req << body;
    return req.str();
}

// ── gzip response decode ──────────────────────────────────────────────────────
// We advertise Accept-Encoding: gzip, so decode it transparently — look/gzip_inflate.h (pure,
// fuzzed: tests/fuzz/fuzz_gzip_inflate.cpp). Bounded output: a gzip bomb cannot exhaust memory.
using look::gzip_inflate;

// Upper limit of a buffered (non-streaming) response — LOOK_HTTP_MAX_RESPONSE bytes, default 64 MB (the same as the gzip
// decode limit). It used to be unbounded: a single http::get to a server sending an endless body
// grew the LOOK process to 15 GB in 25 s (a machine-level process, i.e. it kills the whole site).
// Every feature that fetches a user-supplied address (preview, webhook, import) was exposed to this.
// For large downloads the streaming (callback) path holds no buffer and is not affected by the limit.
static size_t http_max_response() {
    static const size_t v = [] {
        const char* e = std::getenv("LOOK_HTTP_MAX_RESPONSE");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return (size_t)64 << 20;
    }();
    return v;
}
static std::string http_max_response_error() {
    return "http:: response exceeded " + std::to_string(http_max_response()) +
           " bytes (LOOK_HTTP_MAX_RESPONSE); use http::stream() for large downloads";
}

// ── Response parser ───────────────────────────────────────────────────────────

// A chunk-size line: hex digits, optionally followed by spaces/tabs and a ";" chunk extension.
// Exactly what the server accepts in a chunked request, plus the optional whitespace (RFC 9112 BWS).
// std::stoul used to read "1g" as 1 and "-1" as a huge size, so a malformed or hostile response was
// framed differently from a strict reader (response smuggling through a LOOK proxy). At most 15 hex
// digits: a size below 2^60, so `size + 2` cannot overflow.
static bool chunk_size_parse(std::string line, size_t& out) {
    size_t semi = line.find(';');
    if (semi != std::string::npos) line.erase(semi);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (line.empty()) return false;
    size_t lead = line.find_first_not_of('0');                     // leading zeros are allowed
    if (lead != std::string::npos && line.size() - lead > 15) return false;
    size_t v = 0;
    for (char c : line) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + (size_t)d;
    }
    out = v;
    return true;
}

static HttpClientResponse parse_response(const std::string& raw) {
    HttpClientResponse resp;
    if (raw.empty()) { resp.error = "empty response"; return resp; }

    // Status line
    auto crlf1 = raw.find("\r\n");
    if (crlf1 == std::string::npos) { resp.error = "malformed response"; return resp; }
    std::string status_line = raw.substr(0, crlf1);

    // HTTP/1.x NNN ...
    auto sp1 = status_line.find(' ');
    auto sp2 = status_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) { resp.error = "bad status line"; return resp; }
    try {
        resp.status = std::stoi(status_line.substr(sp1 + 1, (sp2 != std::string::npos ? sp2 - sp1 - 1 : std::string::npos)));
    } catch (...) { resp.error = "bad status code"; return resp; }

    // Headers
    size_t pos = crlf1 + 2;
    while (pos < raw.size()) {
        auto end = raw.find("\r\n", pos);
        if (end == std::string::npos) break;
        if (end == pos) { pos += 2; break; }  // blank line = end of headers
        std::string line = raw.substr(pos, end - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v = v.substr(1);
            // lowercase key for case-insensitive lookup
            std::string kl = k;
            std::transform(kl.begin(), kl.end(), kl.begin(), [](unsigned char c){ return std::tolower(c); });
            resp.headers[kl] = v;
        }
        pos = end + 2;
    }

    // Body — handle chunked transfer encoding
    std::string te = resp.headers.count("transfer-encoding") ? resp.headers["transfer-encoding"] : "";
    std::transform(te.begin(), te.end(), te.begin(), [](unsigned char c){ return std::tolower(c); });

    if (te.find("chunked") != std::string::npos) {
        // Decode the chunked body as strictly as the server reads a chunked request (http_server.cpp):
        // a malformed size line, chunk data not followed by CRLF, or a body that ends before the last
        // chunk is an error — it used to give a shorter body silently.
        std::string body_raw = raw.substr(pos);
        std::string body_out;
        size_t p = 0;
        bool complete = false;
        for (;;) {
            size_t cr = body_raw.find("\r\n", p);
            if (cr == std::string::npos) break;                            // ended mid size line
            size_t chunk_size = 0;
            if (!chunk_size_parse(body_raw.substr(p, cr - p), chunk_size)) {
                resp.error = "malformed chunked response: invalid chunk size";
                break;
            }
            p = cr + 2;
            if (chunk_size == 0) { complete = true; break; }                // last chunk (trailers ignored)
            if (body_raw.size() - p < chunk_size + 2) break;                // ended mid chunk
            if (body_raw.compare(p + chunk_size, 2, "\r\n") != 0) {
                resp.error = "malformed chunked response: chunk data is not followed by CRLF";
                break;
            }
            body_out.append(body_raw, p, chunk_size);
            p += chunk_size + 2;
        }
        if (!complete && resp.error.empty()) resp.error = "truncated chunked response";
        resp.body = std::move(body_out);
    } else {
        resp.body = raw.substr(pos);
    }

    // Transparent gzip decode (we advertised Accept-Encoding: gzip).
    auto ce = resp.headers.find("content-encoding");
    if (ce != resp.headers.end() && !resp.body.empty()) {
        std::string enc = ce->second;
        std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c){ return std::tolower(c); });
        if (enc.find("gzip") != std::string::npos) {
            std::string dec;
            if (gzip_inflate(resp.body, 64u * 1024u * 1024u, dec)) {   // 64 MB cap — gzip-bomb guard
                resp.body = dec;
                resp.headers.erase("content-encoding");
            } else {
                resp.error = "http:: gzip decode failed or exceeded the 64 MB decompression cap";
                resp.body.clear();   // don't hand back the still-compressed bytes on failure
            }
        }
    }

    // Transparent charset → UTF-8 for common single-byte encodings declared in the
    // Content-Type header (e.g. "text/html; charset=iso-8859-9").
    auto ct = resp.headers.find("content-type");
    if (ct != resp.headers.end() && !resp.body.empty()) {
        std::string tl = ct->second;
        std::transform(tl.begin(), tl.end(), tl.begin(), [](unsigned char c){ return std::tolower(c); });
        auto cpos = tl.find("charset=");
        if (cpos != std::string::npos) {
            std::string cs = tl.substr(cpos + 8);
            auto semi = cs.find(';'); if (semi != std::string::npos) cs = cs.substr(0, semi);
            while (!cs.empty() && (cs.front() == ' ' || cs.front() == '"')) cs.erase(cs.begin());
            while (!cs.empty() && (cs.back()  == ' ' || cs.back()  == '"')) cs.pop_back();
            if (!cs.empty() && cs != "utf-8" && cs != "utf8" && cs != "us-ascii" && cs != "ascii") {
                std::string conv;
                if (look::charset_to_utf8(resp.body, cs, conv)) resp.body = conv;
            }
        }
    }

    return resp;
}

// ── Streaming: incremental chunked decoder + sink ────────────────────────────
// As body bytes arrive (recv/decrypt/SSL_read) they are fed to this sink. It accumulates until
// the headers are complete, then decodes the chunked encoding and gives it to the callback.

struct ChunkedDecoder {
    std::string buf;
    bool done = false;        // the last chunk arrived
    std::string error;        // malformed stream (the caller reports it; empty = fine so far)
    void feed(const std::string& data, const HttpChunkCallback& emit) {
        buf += data;
        while (!done && error.empty()) {
            size_t cr = buf.find("\r\n");
            if (cr == std::string::npos) return;               // size line is not complete yet
            size_t sz = 0;
            if (!chunk_size_parse(buf.substr(0, cr), sz)) { error = "malformed chunked response: invalid chunk size"; return; }
            if (sz == 0) { done = true; return; }              // last chunk
            if (buf.size() - (cr + 2) < sz + 2) return;        // the whole chunk + CRLF is not here yet
            if (buf.compare(cr + 2 + sz, 2, "\r\n") != 0) {
                error = "malformed chunked response: chunk data is not followed by CRLF";
                return;
            }
            emit(buf.substr(cr + 2, sz));
            buf.erase(0, cr + 2 + sz + 2);
        }
    }
};

struct StreamSink {
    const HttpChunkCallback& cb;
    std::string  head;
    bool         headers_done = false;
    bool         is_chunked   = false;
    HttpClientResponse resp;
    ChunkedDecoder chunked;

    explicit StreamSink(const HttpChunkCallback& c) : cb(c) {}

    void feed(const char* data, size_t n) {
        if (headers_done) { emit(std::string(data, n)); return; }
        head.append(data, n);
        size_t he = head.find("\r\n\r\n");
        if (he == std::string::npos) return;                   // headers are not complete yet
        headers_done = true;
        // Parse the header block (with an empty body) using parse_response — status + headers.
        HttpClientResponse hp = parse_response(head.substr(0, he + 4));
        resp.status  = hp.status;
        resp.headers = hp.headers;
        std::string te = resp.headers.count("transfer-encoding") ? resp.headers["transfer-encoding"] : "";
        std::transform(te.begin(), te.end(), te.begin(), [](unsigned char c){ return std::tolower(c); });
        is_chunked = te.find("chunked") != std::string::npos;
        std::string leftover = head.substr(he + 4);
        head.clear();
        emit(leftover);
    }
    void emit(const std::string& bytes) {
        if (bytes.empty()) return;
        if (is_chunked) chunked.feed(bytes, cb);
        else cb(bytes);
    }
    void finalize() {
        if (!headers_done && resp.error.empty()) resp.error = "no response";
        if (headers_done && is_chunked && resp.error.empty()) {
            if (!chunked.error.empty()) resp.error = chunked.error;
            else if (!chunked.done) resp.error = "truncated chunked response";
        }
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// PLATFORM IMPLEMENTATIONS
// ══════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
// ── Windows: Schannel TLS ─────────────────────────────────────────────────────

struct WsaGuard {
    WsaGuard()  { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
    ~WsaGuard() { WSACleanup(); }
};

static sock_t tcp_connect(const std::string& host, int port, int timeout_ms) {
    static WsaGuard wsa;
    (void)wsa;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    t_conn_error = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        t_conn_error = "DNS resolution failed";
        return INVALID;
    }

    if (is_ssrf_blocked(res)) {
        freeaddrinfo(res);
        t_conn_error = "SSRF protection: request to a private/internal address blocked "
                       "(if needed, LOOK_ALLOW_SSRF=1)";
        return INVALID;
    }

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID; }

    // Timeout
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(s); freeaddrinfo(res); return INVALID;
    }
    freeaddrinfo(res);
    return s;
}

static HttpClientResponse do_plain(const std::string& method, const ParsedUrl& url,
                              const std::string& body,
                              const std::map<std::string, std::string>& hdrs,
                              const HttpOptions& opts,
                              const HttpChunkCallback* on_chunk = nullptr)
{
    HttpClientResponse resp;
    bool via_proxy = !opts.proxy.empty();
    std::string phost; int pport = 0;
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy \"" + opts.proxy + "\" (expected http://host:port)"; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr, /*absolute_uri=*/ via_proxy);
    bool send_tmo = false;
    if (!send_all(s, req.data(), req.size(), &send_tmo)) {
        closesocket(s); resp.error = send_tmo ? "timeout" : "send failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (sink) sink->feed(buf, n);
        else {
            raw.append(buf, n);
            if (raw.size() > http_max_response()) { closesocket(s); resp.error = http_max_response_error(); return resp; }
        }
    }
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) resp.error = "timeout";
        else resp.error = "recv failed";
        closesocket(s);
        return resp;
    }
    closesocket(s);
    // The stream's own error ("no response", a malformed or truncated chunked body) is kept; it used to be
    // overwritten here with resp.error, which is always empty on this path.
    if (sink) { sink->finalize(); if (!resp.error.empty()) sink->resp.error = resp.error; return sink->resp; }
    return parse_response(raw);
}

static HttpClientResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts,
                            const HttpChunkCallback* on_chunk = nullptr)
{
    HttpClientResponse resp;
    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    bool via_proxy = !opts.proxy.empty();
    std::string phost; int pport = 0;
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy \"" + opts.proxy + "\" (expected http://host:port)"; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    if (via_proxy) {
        // CONNECT tunnel to the target through the proxy, then run TLS over the tunnel.
        std::string tgt = url_authority(url, true);   // IPv6 in square brackets
        std::string creq = "CONNECT " + tgt + " HTTP/1.1\r\nHost: " + tgt + "\r\n\r\n";
        bool ctmo = false;
        if (!send_all(s, creq.data(), creq.size(), &ctmo)) { closesocket(s); resp.error = "proxy CONNECT send failed"; return resp; }
        std::string pr; char pb[1024]; int pn;
        while (pr.find("\r\n\r\n") == std::string::npos) {
            pn = recv(s, pb, sizeof(pb), 0);
            if (pn <= 0) { closesocket(s); resp.error = "proxy closed during CONNECT"; return resp; }
            pr.append(pb, pn);
            if (pr.size() > 16384) { closesocket(s); resp.error = "proxy CONNECT response too large"; return resp; }
        }
        int pstatus = 0; auto sp = pr.find(' ');
        if (sp != std::string::npos) { try { pstatus = std::stoi(pr.substr(sp + 1, 3)); } catch (...) {} }
        if (pstatus < 200 || pstatus >= 300) { closesocket(s); resp.error = "http:: proxy CONNECT rejected (status " + std::to_string(pstatus) + ")"; return resp; }
    }

    // Schannel credential
    SCHANNEL_CRED sc_cred{};
    sc_cred.dwVersion = SCHANNEL_CRED_VERSION;
    sc_cred.dwFlags   = SCH_CRED_AUTO_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;

    CredHandle cred_handle;
    TimeStamp  cred_ts;
    if (AcquireCredentialsHandleA(nullptr, (LPSTR)UNISP_NAME_A,
                                   SECPKG_CRED_OUTBOUND, nullptr,
                                   &sc_cred, nullptr, nullptr,
                                   &cred_handle, &cred_ts) != SEC_E_OK)
    { closesocket(s); resp.error = "TLS credential failed"; return resp; }

    // TLS handshake
    CtxtHandle ctx_handle;
    SecBuffer   out_buf_desc[1]{};
    SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, out_buf_desc };
    out_buf_desc[0].BufferType = SECBUFFER_TOKEN;

    std::wstring whost(url.host.begin(), url.host.end());
    ULONG ctx_attrs = 0;
    TimeStamp ctx_ts;

    SecBuffer   in_bufs[2]{};
    SecBufferDesc in_desc{ SECBUFFER_VERSION, 2, in_bufs };
    std::string handshake_buf;
    bool first = true;
    bool ctx_inited = false;

    for (;;) {
        out_buf_desc[0] = {};
        out_buf_desc[0].BufferType = SECBUFFER_TOKEN;
        out_desc = { SECBUFFER_VERSION, 1, out_buf_desc };

        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].pvBuffer   = handshake_buf.empty() ? nullptr : (void*)handshake_buf.data();
        in_bufs[0].cbBuffer   = (ULONG)handshake_buf.size();
        in_bufs[1].BufferType = SECBUFFER_EMPTY;

        SECURITY_STATUS ss;
        if (first) {
            ss = InitializeSecurityContextA(
                &cred_handle, nullptr, (LPSTR)url.host.c_str(),
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                0, 0, nullptr, 0, &ctx_handle, &out_desc, &ctx_attrs, &ctx_ts);
            first = false;
            ctx_inited = true;
        } else {
            ss = InitializeSecurityContextA(
                &cred_handle, &ctx_handle, nullptr,
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                0, 0, &in_desc, 0, nullptr, &out_desc, &ctx_attrs, &ctx_ts);
        }

        if (out_buf_desc[0].pvBuffer && out_buf_desc[0].cbBuffer > 0) {
            send(s, (const char*)out_buf_desc[0].pvBuffer, (int)out_buf_desc[0].cbBuffer, 0);
            FreeContextBuffer(out_buf_desc[0].pvBuffer);
        }

        if (ss == SEC_E_OK || ss == SEC_I_CONTEXT_EXPIRED) break;
        if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_I_INCOMPLETE_CREDENTIALS) {
            // Read more from server
            char rbuf[16384];
            // Carry over any unprocessed bytes. For InitializeSecurityContext, Schannel
            // reports SECBUFFER_EXTRA by count only — pvBuffer is NOT set; the extra bytes
            // are the LAST cbBuffer bytes of the input we passed. Copy them from the end of
            // the OLD handshake_buf BEFORE clearing it (reading in_bufs[1].pvBuffer here was a
            // use-of-unset-pointer crash that fragmented handshakes — e.g. via a proxy — hit).
            std::string carry;
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0
                && in_bufs[1].cbBuffer <= handshake_buf.size()) {
                carry.assign(handshake_buf.data() + (handshake_buf.size() - in_bufs[1].cbBuffer),
                             in_bufs[1].cbBuffer);
            }
            handshake_buf.swap(carry);
            int n = recv(s, rbuf, sizeof(rbuf), 0);
            if (n <= 0) { resp.error = "TLS handshake recv failed"; goto cleanup; }
            handshake_buf.append(rbuf, n);
            continue;
        }
        // Any other HRESULT = failure
        resp.error = "TLS handshake failed";
        goto cleanup;
    }

    {
        // Query stream sizes
        SecPkgContext_StreamSizes stream_sizes{};
        QueryContextAttributesA(&ctx_handle, SECPKG_ATTR_STREAM_SIZES, &stream_sizes);

        // Encrypt request
        std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr);
        std::string enc_buf(stream_sizes.cbHeader + req.size() + stream_sizes.cbTrailer, '\0');
        memcpy((char*)enc_buf.data() + stream_sizes.cbHeader, req.data(), req.size());

        SecBuffer enc_bufs[4]{};
        enc_bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        enc_bufs[0].pvBuffer   = (void*)enc_buf.data();
        enc_bufs[0].cbBuffer   = stream_sizes.cbHeader;
        enc_bufs[1].BufferType = SECBUFFER_DATA;
        enc_bufs[1].pvBuffer   = (char*)enc_buf.data() + stream_sizes.cbHeader;
        enc_bufs[1].cbBuffer   = (ULONG)req.size();
        enc_bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        enc_bufs[2].pvBuffer   = (char*)enc_buf.data() + stream_sizes.cbHeader + req.size();
        enc_bufs[2].cbBuffer   = stream_sizes.cbTrailer;
        enc_bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc enc_desc{ SECBUFFER_VERSION, 4, enc_bufs };

        // The result of encryption and sending is checked: both used to be ignored (a failed
        // encryption or a partial send passed silently and a response was awaited).
        if (EncryptMessage(&ctx_handle, 0, &enc_desc, 0) != SEC_E_OK) {
            resp.error = "TLS encrypt failed"; goto cleanup2;
        }
        size_t total = enc_bufs[0].cbBuffer + enc_bufs[1].cbBuffer + enc_bufs[2].cbBuffer;
        bool etmo = false;
        if (!send_all(s, enc_buf.data(), total, &etmo)) {
            resp.error = etmo ? "timeout" : "send failed"; goto cleanup2;
        }

        // Receive + decrypt
        std::string raw_enc;
        char rbuf2[16384];
        std::string decrypted;
        bool done = false;

        while (!done) {
            int n = recv(s, rbuf2, sizeof(rbuf2), 0);
            if (n == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAETIMEDOUT) { resp.error = "timeout"; goto cleanup2; }
                break;
            }
            if (n == 0) break;
            raw_enc.append(rbuf2, n);

            // Try to decrypt what we have
            while (!raw_enc.empty()) {
                SecBuffer dec_bufs[4]{};
                dec_bufs[0].BufferType = SECBUFFER_DATA;
                dec_bufs[0].pvBuffer   = (void*)raw_enc.data();
                dec_bufs[0].cbBuffer   = (ULONG)raw_enc.size();
                dec_bufs[1].BufferType = SECBUFFER_EMPTY;
                dec_bufs[2].BufferType = SECBUFFER_EMPTY;
                dec_bufs[3].BufferType = SECBUFFER_EMPTY;
                SecBufferDesc dec_desc{ SECBUFFER_VERSION, 4, dec_bufs };

                SECURITY_STATUS ds = DecryptMessage(&ctx_handle, &dec_desc, 0, nullptr);
                if (ds == SEC_E_INCOMPLETE_MESSAGE) break;  // need more data
                if (ds == SEC_I_CONTEXT_EXPIRED || ds == SEC_I_RENEGOTIATE) { done = true; break; }
                if (ds != SEC_E_OK) { done = true; break; }

                // Find decrypted data buffer
                for (int i = 0; i < 4; ++i) {
                    if (dec_bufs[i].BufferType == SECBUFFER_DATA && dec_bufs[i].pvBuffer) {
                        if (sink) sink->feed((char*)dec_bufs[i].pvBuffer, dec_bufs[i].cbBuffer);
                        else decrypted.append((char*)dec_bufs[i].pvBuffer, dec_bufs[i].cbBuffer);
                    }
                }
                // Extra data after this record
                std::string leftover;
                for (int i = 0; i < 4; ++i) {
                    if (dec_bufs[i].BufferType == SECBUFFER_EXTRA && dec_bufs[i].pvBuffer)
                        leftover.append((char*)dec_bufs[i].pvBuffer, dec_bufs[i].cbBuffer);
                }
                raw_enc = leftover;
            }
        }
        if (sink) { sink->finalize(); resp = sink->resp; }
        else resp = parse_response(decrypted);
        goto cleanup2;
    }

cleanup:
    if (ctx_inited) DeleteSecurityContext(&ctx_handle);
    FreeCredentialsHandle(&cred_handle);
    closesocket(s);
    return resp;

cleanup2:
    DeleteSecurityContext(&ctx_handle);
    FreeCredentialsHandle(&cred_handle);
    closesocket(s);
    return resp;
}

#else
// ── Linux: OpenSSL ────────────────────────────────────────────────────────────

static sock_t tcp_connect(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    // t_conn_error is written ONLY after the last suspension point: DNS and connect suspend the fiber,
    // and meanwhile another request on the same thread may write its own reason into t_conn_error.
    // There is no suspension between the return and the caller's read → the value read is this request's.
    if (look::fiber_getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        t_conn_error = "DNS resolution failed";
        return INVALID;
    }

    if (is_ssrf_blocked(res)) {
        freeaddrinfo(res);
        t_conn_error = "SSRF protection: request to a private/internal address blocked "
                       "(if needed, LOOK_ALLOW_SSRF=1)";
        return INVALID;
    }

    // The resolved addresses are tried in order: "localhost" returns ::1 first; a service listening only on IPv4
    // used to be unreachable (only the first address was tried).
    sock_t s = INVALID;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) { s = INVALID; continue; }
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // While a connection is being established inside a fiber only the fiber waits (at most timeout_ms).
        if (look::fiber_connect(s, p->ai_addr, p->ai_addrlen, timeout_ms) == 0) break;
        close(s); s = INVALID;
    }
    freeaddrinfo(res);
    t_conn_error = nullptr;
    return s;
}

// OpenSSL context — initialized once
struct SslCtxGuard {
    SSL_CTX* ctx = nullptr;
    SslCtxGuard() {
        configure_system_ca_bundle();   // find the system CA for static OpenSSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ctx = SSL_CTX_new(TLS_client_method());
        look::tls_client_defaults(ctx);   // at least TLS 1.2, machine-independent
        if (ctx) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);   // reads SSL_CERT_FILE/DIR
        }
    }
    ~SslCtxGuard() { if (ctx) SSL_CTX_free(ctx); }
};

static SSL_CTX* get_ssl_ctx() {
    static SslCtxGuard guard;
    return guard.ctx;
}

// ── Fiber-aware waiting (Go netpoller) ──────────────────────────────────────────
// When http::get is called from a web route, while waiting for a SLOW external API's response
// (recv/SSL_read) it must not block the worker thread: if we are inside a fiber we YIELD until the fd is
// readable (the scheduler runs another goroutine), and epoll resumes when data arrives.
// This opens up the fiber's one real value proposition, "concurrent external I/O".
// DNS, connect, the TLS handshake and sending only make the fiber wait, in the same way as the look/fiber_net.h helpers.
// The socket stays blocking → the read after the yield returns immediately.
// Return: 1 = readable (read), 0 = timeout (the external API did not respond), -1 = no fiber/epoll
// (the caller falls back to the existing plain-blocking behavior → the DEFAULT pool path is preserved exactly).
static int client_wait_readable(int fd, int timeout_ms) {
#ifdef __linux__
    look::FiberScheduler* sched = look::get_thread_scheduler();
    look::Fiber*          cur   = look::Fiber::current();
    if (sched && cur) {
        auto self = cur->shared_from_this_fiber();
        if (self) return sched->wait_readable_tmo(self, fd, timeout_ms > 0 ? timeout_ms : 30000);
    }
#else
    (void)fd; (void)timeout_ms;
#endif
    return -1;  // outside a fiber → the caller does a blocking read (existing behavior)
}

static HttpClientResponse do_plain(const std::string& method, const ParsedUrl& url,
                              const std::string& body,
                              const std::map<std::string, std::string>& hdrs,
                              const HttpOptions& opts,
                              const HttpChunkCallback* on_chunk = nullptr)
{
    HttpClientResponse resp;
    bool via_proxy = !opts.proxy.empty();
    std::string phost; int pport = 0;
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy \"" + opts.proxy + "\" (expected http://host:port)"; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr, /*absolute_uri=*/ via_proxy);
    // All of it is sent (it used to be a single send: on a partial write the rest of the request was silently not sent).
    if (!look::fiber_send_all(s, nullptr, req.data(), req.size(), opts.timeout_ms)) {
        close(s); resp.error = "send failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    ssize_t n;
    for (;;) {
        // If we are inside a fiber: YIELD while waiting for data (do not block the worker). Outside a fiber
        // client_wait_readable returns -1 → straight blocking recv (existing behavior exactly).
        int w = client_wait_readable(s, opts.timeout_ms);
        if (w == 0) { resp.error = "timeout"; break; }
        n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) {
            if (sink) { sink->feed(buf, n); continue; }
            raw.append(buf, n);
            if (raw.size() > http_max_response()) { resp.error = http_max_response_error(); break; }
            continue;
        }
        if (n < 0) resp.error = "timeout";
        break;
    }
    close(s);
    if (!resp.error.empty()) return resp;
    if (sink) { sink->finalize(); return sink->resp; }
    return parse_response(raw);
}

static HttpClientResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts,
                            const HttpChunkCallback* on_chunk = nullptr)
{
    HttpClientResponse resp;
    SSL_CTX* ctx = get_ssl_ctx();
    if (!ctx) { resp.error = "SSL_CTX init failed"; return resp; }

    bool via_proxy = !opts.proxy.empty();
    std::string phost; int pport = 0;
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy \"" + opts.proxy + "\" (expected http://host:port)"; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    if (via_proxy) {
        // CONNECT tunnel to the target through the proxy, then run TLS over the tunnel.
        std::string tgt = url_authority(url, true);   // IPv6 in square brackets
        std::string creq = "CONNECT " + tgt + " HTTP/1.1\r\nHost: " + tgt + "\r\n\r\n";
        if (!look::fiber_send_all(s, nullptr, creq.data(), creq.size(), opts.timeout_ms)) { close(s); resp.error = "proxy CONNECT send failed"; return resp; }
        std::string pr; char pb[1024];
        for (;;) {
            int w = client_wait_readable(s, opts.timeout_ms);
            if (w == 0) { close(s); resp.error = "proxy CONNECT timeout"; return resp; }
            ssize_t pn = recv(s, pb, sizeof(pb), 0);
            if (pn <= 0) { close(s); resp.error = "proxy closed during CONNECT"; return resp; }
            pr.append(pb, pn);
            if (pr.find("\r\n\r\n") != std::string::npos) break;
            if (pr.size() > 16384) { close(s); resp.error = "proxy CONNECT response too large"; return resp; }
        }
        int pstatus = 0; auto sp = pr.find(' ');
        if (sp != std::string::npos) { try { pstatus = std::stoi(pr.substr(sp + 1, 3)); } catch (...) {} }
        if (pstatus < 200 || pstatus >= 300) { close(s); resp.error = "http:: proxy CONNECT rejected (status " + std::to_string(pstatus) + ")"; return resp; }
    }

    SSL* ssl = SSL_new(ctx);
    if (!ssl) { close(s); resp.error = "SSL_new failed"; return resp; }

    SSL_set_fd(ssl, s);
    // SNI + name verification (IP → IP SAN, no SNI — Go crypto/tls); if it cannot be set up, do not connect.
    if (!look::tls_set_peer(ssl, url.host, true)) {
        SSL_free(ssl); close(s);
        resp.error = "http:: could not set up TLS name verification for " + url.host;
        return resp;
    }

    if (int hs = look::fiber_ssl_connect(ssl, s, opts.timeout_ms); hs != 1) {
        // The reason goes to the user (it used to be only "TLS handshake failed": an expired certificate, a wrong name,
        // an unknown CA could not be told apart).
        std::string why = hs == 0 ? std::string("timeout") : look::tls_failure_reason(ssl);
        SSL_free(ssl); close(s);
        resp.error = "TLS handshake failed" + (why.empty() ? std::string() : ": " + why);
        return resp;
    }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr);
    if (!look::fiber_send_all(s, ssl, req.data(), req.size(), opts.timeout_ms)) {
        SSL_free(ssl); close(s);
        resp.error = "SSL_write failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    int n;
    for (;;) {
        // Fiber-aware yield: if SSL_pending>0 there are decrypted bytes in OpenSSL's buffer →
        // the fd may not be readable, DO NOT WAIT (otherwise it deadlocks). If the buffer is empty, yield until the fd is
        // readable. Since the socket stays blocking, SSL_read reads after the yield;
        // bounded by SO_RCVTIMEO. Outside a fiber client_wait_readable is -1 → SSL_read immediately.
        if (SSL_pending(ssl) == 0) {
            int w = client_wait_readable(s, opts.timeout_ms);
            if (w == 0) { resp.error = "timeout"; break; }
        }
        n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) {
            if (sink) { sink->feed(buf, n); continue; }
            raw.append(buf, n);
            if (raw.size() > http_max_response()) { resp.error = http_max_response_error(); break; }
            continue;
        }
        if (n < 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;  // continue → yield
            if (err == SSL_ERROR_SYSCALL) resp.error = "timeout";
        }
        break;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(s);
    if (!resp.error.empty()) return resp;
    if (sink) { sink->finalize(); return sink->resp; }
    return parse_response(raw);
}

#endif // _WIN32

// ── Public API ────────────────────────────────────────────────────────────────

// Resolve a (possibly relative) Location header against the current URL.
// Handles absolute, scheme-relative (//host/path) and path-absolute (/path),
// plus a basic relative path. Returns "" when it can't be resolved.
static std::string resolve_url(const std::string& base, const std::string& loc) {
    if (loc.empty()) return "";
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;

    std::string scheme, rest;
    if      (base.rfind("https://", 0) == 0) { scheme = "https"; rest = base.substr(8); }
    else if (base.rfind("http://",  0) == 0) { scheme = "http";  rest = base.substr(7); }
    else return "";

    std::string authority = rest.substr(0, rest.find_first_of("/?#"));   // [userinfo@]host[:port]
    if (loc.rfind("//", 0) == 0) return scheme + ":" + loc;      // scheme-relative
    if (loc[0] == '/')           return scheme + "://" + authority + loc;  // path-absolute

    // relative to the base directory
    std::string p = rest.substr(authority.size());              // "/dir/file?q"
    auto q = p.find('?'); if (q != std::string::npos) p = p.substr(0, q);
    auto slash = p.rfind('/');
    std::string dir = (slash == std::string::npos) ? "/" : p.substr(0, slash + 1);
    return scheme + "://" + authority + dir + loc;
}

// scheme + lowercased host[:port] of a URL — the "origin" for credential-scoping.
// via parse_url (scheme + lowercase host + port): credentials, '?' or '#' do not get mixed into the origin.
static std::string url_origin(const std::string& u) {
    try {
        ParsedUrl p = parse_url(u);
        std::string h = p.host;
        for (char& c : h) c = (char)std::tolower((unsigned char)c);
        return std::string(p.tls ? "https://" : "http://") + h + ":" + std::to_string(p.port);
    } catch (...) { return u; }
}

// Erase a header by name, case-insensitively (user-supplied keys are arbitrary case).
static void erase_header_ci(std::map<std::string, std::string>& h, const std::string& name) {
    for (auto it = h.begin(); it != h.end(); ) {
        if (it->first.size() == name.size() &&
            std::equal(it->first.begin(), it->first.end(), name.begin(),
                       [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
            it = h.erase(it);
        else ++it;
    }
}

HttpClientResponse http_request(const std::string& method,
                           const std::string& url_str,
                           const std::string& body,
                           const std::map<std::string, std::string>& req_headers,
                           const HttpOptions& opts)
{
    HttpClientResponse resp;
    std::string cur_url    = url_str;
    std::string cur_method = method;
    std::string cur_body   = body;
    std::map<std::string, std::string> cur_headers = req_headers;

    int max_hops = opts.follow_redirects ? (opts.max_redirects > 0 ? opts.max_redirects : 5) : 0;
    std::vector<std::string> seen;

    for (int hop = 0; ; ++hop) {
        ParsedUrl url;
        try { url = parse_url(cur_url); }
        catch (std::exception& e) { resp.error = e.what(); return resp; }

        try {
            // Each hop reconnects, so tcp_connect's SSRF guard re-runs for the new host.
            if (url.tls) resp = do_tls(cur_method, url, cur_body, cur_headers, opts);
            else         resp = do_plain(cur_method, url, cur_body, cur_headers, opts);
        } catch (std::exception& e) { resp.error = e.what(); return resp; }

        if (!resp.error.empty()) return resp;
        if (max_hops <= 0 || hop >= max_hops) return resp;
        if (resp.status < 300 || resp.status >= 400) return resp;

        auto it = resp.headers.find("location");
        if (it == resp.headers.end() || it->second.empty()) return resp;

        std::string next = resolve_url(cur_url, it->second);
        if (next.empty()) return resp;                              // unresolvable → stop
        if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
            resp.error = "http:: redirect loop detected"; return resp;
        }
        seen.push_back(next);

        // Cross-origin hop: drop credential headers so an open-redirect on a trusted host
        // can't exfiltrate them to the target (curl/browser behaviour).
        if (url_origin(next) != url_origin(cur_url)) {
            erase_header_ci(cur_headers, "Authorization");
            erase_header_ci(cur_headers, "Cookie");
            erase_header_ci(cur_headers, "Proxy-Authorization");
        }

        // 303 (and POST-style 301/302) become GET without a body, per common practice.
        if (resp.status == 303 ||
            ((resp.status == 301 || resp.status == 302) && cur_method != "GET" && cur_method != "HEAD")) {
            cur_method = "GET";
            cur_body.clear();
            cur_headers.erase("Content-Type");
            cur_headers.erase("Content-Length");
        }
        cur_url = next;
    }
}

HttpClientResponse http_request_stream(const std::string& method,
                                  const std::string& url_str,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& req_headers,
                                  const HttpOptions& opts,
                                  const HttpChunkCallback& on_chunk)
{
    HttpClientResponse resp;
    ParsedUrl url;
    try { url = parse_url(url_str); }
    catch (std::exception& e) { resp.error = e.what(); return resp; }

    try {
        if (url.tls) return do_tls(method, url, body, req_headers, opts, &on_chunk);
        else         return do_plain(method, url, body, req_headers, opts, &on_chunk);
    } catch (std::exception& e) {
        resp.error = e.what();
        return resp;
    }
}

} // namespace look

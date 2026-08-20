// http_client.cpp — self-contained HTTP/HTTPS client
// Linux: OpenSSL (system libssl)
// Windows: Schannel (built-in, WinSSL)

#include "look/http_client.h"
#include "look/ssrf_safe.h"   // SSRF private/özel-blok kararı (saf, tablo-test edilebilir)
#include "miniz/miniz.h"      // gzip response decode (raw deflate; header parsed by hand)
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
#  include "look/fiber.h"   // fiber-aware recv (Go netpoller) — yalnız Linux --mode http
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
typedef int sock_t;
static const sock_t INVALID = -1;
#endif

namespace look {

// ── Sistem CA bundle'ını bul (statik OpenSSL için — her iki platformda tanımlı) ──
// Statik OpenSSL derleme-prefix'indeki CA dizinini arar (hedefte YOK) → https
// doğrulaması sessizce başarısız olur. Standart yolları probe edip SSL_CERT_FILE/DIR
// ayarlarız. Zaten set ise dokunmayız (OS/crypto-policies). Windows: Schannel sistem
// cert store'u kullanır → no-op.
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

// ── SSRF koruması — private/loopback IP'lere bağlantı engeli ─────────────────

// Karar mantığı look/ssrf_safe.h'de (saf, tablo-test edilebilir). Buradaki
// döngü yalnız addrinfo → (family, ham byte) çıkarımı yapıp predikata delege eder.
static bool is_ssrf_blocked(const struct addrinfo* res) {
    // LOOK_ALLOW_SSRF=1 ile devre dışı bırakılabilir (iç ağ test ortamı için)
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

// Bağlantı hatasının NEDENİ — tcp_connect'ten çağırana taşınır.
// ESKİ HATA: tcp_connect DNS hatası, SSRF engeli, socket hatası ve gerçek bağlantı
// hatası için AYNI INVALID'i döndürüyordu; çağıran taraf da hepsine "connection
// failed" diyordu. Sonuç: SSRF ENGELİ ile GERÇEK AĞ HATASI ayırt edilemiyordu —
// iç servisine ulaşamayan geliştirici isteği bir güvenlik politikasının
// engellediğini göremiyor, boşuna ağ/DNS/firewall araştırıyordu.
// (33. bug ile aynı sınıf: davranış doğru, mesaj teşhis ettirmiyor.)
static thread_local const char* t_conn_error = nullptr;

// ── URL parser ────────────────────────────────────────────────────────────────

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl r;
    std::string s = url;

    if (s.substr(0, 8) == "https://") { r.tls = true;  s = s.substr(8); r.port = 443; }
    else if (s.substr(0, 7) == "http://") { r.tls = false; s = s.substr(7); r.port = 80; }
    else throw std::runtime_error("http:: Unsupported URL scheme: " + url);

    auto path_pos = s.find('/');
    std::string host_port = (path_pos != std::string::npos) ? s.substr(0, path_pos) : s;
    r.path = (path_pos != std::string::npos) ? s.substr(path_pos) : "/";

    // IPv6 literal: RFC 3986 gereği köşeli parantez içinde — "[::1]", "[::1]:8080".
    // ESKİ HATA: parantezler sıyrılmıyordu ve port ayracı `rfind(':')` ile
    // aranıyordu. Sonuç:
    //   "[::1]:8080" -> host "[::1]"   (parantezli → getaddrinfo başarısız)
    //   "[::1]"      -> host "[:"      (adresin İÇİNDEKİ iki nokta port sanıldı)
    // Yani hiçbir IPv6 literal URL'i çalışmıyordu; hata da "DNS çözümlenemedi"
    // olduğu için sebep görünmüyordu (bu bug zaten gizliydi; SSRF hata mesajları
    // ayrıştırılınca ortaya çıktı).
    if (!host_port.empty() && host_port[0] == '[') {
        auto rb = host_port.find(']');
        if (rb == std::string::npos)
            throw std::runtime_error("http:: Invalid IPv6 URL (missing closing ']'): " + url);
        r.host = host_port.substr(1, rb - 1);          // parantezler host'a DAHİL DEĞİL
        if (rb + 1 < host_port.size() && host_port[rb + 1] == ':') {
            try { r.port = std::stoi(host_port.substr(rb + 2)); }
            catch (...) { r.port = r.tls ? 443 : 80; }  // bozuk port → varsayılan
        }
        return r;
    }

    auto colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        r.host = host_port.substr(0, colon);
        try { r.port = std::stoi(host_port.substr(colon + 1)); }  // bozuk port → default
        catch (...) { r.port = r.tls ? 443 : 80; }
    } else {
        r.host = host_port;
    }
    return r;
}

// ── Request builder ───────────────────────────────────────────────────────────

// CRLF sıyırma — giden isteğe request-splitting / header injection engeli.
// Kullanıcı-kontrollü URL yolu, host veya header değeri \r\n içerirse sahte
// header ya da istek sınırı enjekte edemesin (sunucu tarafı response header'da
// aynı savunmayı yapıyor; client de aynı disiplini uygular).
static std::string hc_strip_crlf(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) if (c != '\r' && c != '\n') r += c;
    return r;
}

// Parse a proxy spec ("http://host:port", "host:port", "host") into host + port.
static bool parse_proxy(const std::string& spec, std::string& host, int& port) {
    std::string s = spec;
    if (s.rfind("http://", 0) == 0)       s = s.substr(7);
    else if (s.rfind("https://", 0) == 0) s = s.substr(8);
    auto slash = s.find('/'); if (slash != std::string::npos) s = s.substr(0, slash);
    port = 8080;
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        const std::string ps = s.substr(colon + 1);
        if (ps.empty() || ps.find_first_not_of("0123456789") != std::string::npos) return false;
        try { port = std::stoi(ps); } catch (...) { return false; }
    } else {
        host = s;
    }
    return !host.empty() && port > 0 && port < 65536;
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
    if (absolute_uri) {
        std::ostringstream u;
        u << (url.tls ? "https://" : "http://") << hc_strip_crlf(url.host);
        if ((url.tls && url.port != 443) || (!url.tls && url.port != 80)) u << ":" << url.port;
        u << hc_strip_crlf(url.path);
        target = u.str();
    }
    req << hc_strip_crlf(method) << " " << target << " HTTP/1.1\r\n";
    req << "Host: " << hc_strip_crlf(url.host);
    if ((url.tls && url.port != 443) || (!url.tls && url.port != 80))
        req << ":" << url.port;
    req << "\r\n";
    req << "User-Agent: LOOK/0.19\r\n";
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
// We advertise Accept-Encoding: gzip, so decode it transparently. The gzip header
// is parsed by hand and the payload is raw-inflated with miniz (window_bits < 0),
// which avoids depending on miniz's optional gzip-wrapper support. Bounded output:
// decompression stops and fails past `cap`, so a gzip bomb cannot exhaust memory.
static bool gzip_inflate(const std::string& in, size_t cap, std::string& out) {
    if (in.size() < 18) return false;                       // 10-byte hdr + 8-byte trailer min
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08) return false;  // magic + DEFLATE method
    unsigned char flg = p[3];
    size_t off = 10;
    if (flg & 0x04) {                                       // FEXTRA
        if (off + 2 > in.size()) return false;
        size_t xlen = (size_t)p[off] | ((size_t)p[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) { while (off < in.size() && p[off] != 0) ++off; ++off; }  // FNAME
    if (flg & 0x10) { while (off < in.size() && p[off] != 0) ++off; ++off; }  // FCOMMENT
    if (flg & 0x02) off += 2;                               // FHCRC
    if (off >= in.size()) return false;

    mz_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (mz_inflateInit2(&strm, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;  // raw deflate
    strm.next_in  = p + off;
    strm.avail_in = (unsigned int)(in.size() - off);        // trailer left over; inflate stops at stream end

    out.clear();
    char obuf[16384];
    int ret;
    do {
        do {
            strm.next_out  = reinterpret_cast<unsigned char*>(obuf);
            strm.avail_out = sizeof(obuf);
            ret = mz_inflate(&strm, MZ_NO_FLUSH);
            if (ret != MZ_OK && ret != MZ_STREAM_END && ret != MZ_BUF_ERROR) {
                mz_inflateEnd(&strm); return false;
            }
            out.append(obuf, sizeof(obuf) - strm.avail_out);
            if (out.size() > cap) { mz_inflateEnd(&strm); return false; }  // bomb guard
        } while (strm.avail_out == 0 && ret != MZ_STREAM_END);
    } while (ret != MZ_STREAM_END && strm.avail_in > 0);
    mz_inflateEnd(&strm);
    return ret == MZ_STREAM_END;
}

// ── Response parser ───────────────────────────────────────────────────────────

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
        // Decode chunked body
        std::string body_raw = raw.substr(pos);
        std::ostringstream body_out;
        size_t p = 0;
        while (p < body_raw.size()) {
            auto cr = body_raw.find("\r\n", p);
            if (cr == std::string::npos) break;
            std::string hex = body_raw.substr(p, cr - p);
            // strip chunk extensions
            auto semi = hex.find(';');
            if (semi != std::string::npos) hex = hex.substr(0, semi);
            size_t chunk_size = 0;
            try { chunk_size = std::stoul(hex, nullptr, 16); } catch (...) { break; }
            if (chunk_size == 0) break;
            p = cr + 2;
            body_out << body_raw.substr(p, chunk_size);
            p += chunk_size + 2;
        }
        resp.body = body_out.str();
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

// ── Streaming: artimli chunked-decoder + sink ────────────────────────────────
// Govde baytlari geldikce (recv/decrypt/SSL_read) bu sink'e beslenir. Once
// basliklar tamamlanana kadar biriktirir, sonra chunked'i cozup callback'e verir.

struct ChunkedDecoder {
    std::string buf;
    bool done = false;
    void feed(const std::string& data, const HttpChunkCallback& emit) {
        buf += data;
        while (!done) {
            size_t cr = buf.find("\r\n");
            if (cr == std::string::npos) return;               // boyut satiri henuz tam degil
            std::string hex = buf.substr(0, cr);
            size_t semi = hex.find(';');
            if (semi != std::string::npos) hex = hex.substr(0, semi);  // chunk-extension at
            while (!hex.empty() && (hex.back() == ' ' || hex.back() == '\t')) hex.pop_back();
            size_t sz = 0;
            try { sz = std::stoul(hex, nullptr, 16); } catch (...) { done = true; return; }
            if (sz == 0) { done = true; return; }              // son chunk
            if (buf.size() < cr + 2 + sz + 2) return;          // tum chunk + CRLF henuz yok
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
        if (he == std::string::npos) return;                   // basliklar henuz tam degil
        headers_done = true;
        // Baslik blogunu (bos govdeyle) parse_response ile coz — status + headers.
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
        t_conn_error = "DNS cozumlenemedi";
        return INVALID;
    }

    if (is_ssrf_blocked(res)) {
        freeaddrinfo(res);
        t_conn_error = "SSRF korumasi: ozel/ic ag adresine istek engellendi "
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
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy: " + opts.proxy; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr, /*absolute_uri=*/ via_proxy);
    if (send(s, req.c_str(), (int)req.size(), 0) == SOCKET_ERROR) {
        closesocket(s); resp.error = "send failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (sink) sink->feed(buf, n); else raw.append(buf, n);
    }
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) resp.error = "timeout";
        else resp.error = "recv failed";
        closesocket(s);
        return resp;
    }
    closesocket(s);
    if (sink) { sink->finalize(); sink->resp.error = resp.error; return sink->resp; }
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
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy: " + opts.proxy; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    if (via_proxy) {
        // CONNECT tunnel to the target through the proxy, then run TLS over the tunnel.
        std::string tgt = hc_strip_crlf(url.host) + ":" + std::to_string(url.port);
        std::string creq = "CONNECT " + tgt + " HTTP/1.1\r\nHost: " + tgt + "\r\n\r\n";
        if (send(s, creq.c_str(), (int)creq.size(), 0) == SOCKET_ERROR) { closesocket(s); resp.error = "proxy CONNECT send failed"; return resp; }
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

        EncryptMessage(&ctx_handle, 0, &enc_desc, 0);
        size_t total = enc_bufs[0].cbBuffer + enc_bufs[1].cbBuffer + enc_bufs[2].cbBuffer;
        send(s, enc_buf.data(), (int)total, 0);

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

    t_conn_error = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        t_conn_error = "DNS cozumlenemedi";
        return INVALID;
    }

    if (is_ssrf_blocked(res)) {
        freeaddrinfo(res);
        t_conn_error = "SSRF korumasi: ozel/ic ag adresine istek engellendi "
                       "(if needed, LOOK_ALLOW_SSRF=1)";
        return INVALID;
    }

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return INVALID; }

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s); freeaddrinfo(res); return INVALID;
    }
    freeaddrinfo(res);
    return s;
}

// OpenSSL context — initialized once
struct SslCtxGuard {
    SSL_CTX* ctx = nullptr;
    SslCtxGuard() {
        configure_system_ca_bundle();   // statik OpenSSL için sistem CA'sını bul
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);   // SSL_CERT_FILE/DIR'i okur
        }
    }
    ~SslCtxGuard() { if (ctx) SSL_CTX_free(ctx); }
};

static SSL_CTX* get_ssl_ctx() {
    static SslCtxGuard guard;
    return guard.ctx;
}

// ── Fiber-aware bekleme (Go netpoller) ──────────────────────────────────────────
// http::get bir web route'undan çağrıldığında, YAVAŞ dış API'nin yanıtını beklerken
// (recv/SSL_read) worker thread'ini bloklamamalı: fiber içindeysek fd okunabilir olana
// dek YIELD ederiz (scheduler başka goroutine koşturur), veri gelince epoll resume eder.
// Bu, fiber'in tek gerçek değer önerisi olan "eşzamanlı dış I/O"yu açar (bkz. PROJE_DURUMU
// §9). Kapsam BİLİNÇLİ olarak recv yoluyla sınırlı: connect/handshake/send zaten hızlı ve
// SO_*TIMEO ile sınırlı; onları non-blocking'e çevirmek risk/getiri açısından değmez
// (wait_writable yok). Soket blocking kalır → yield'den sonraki okuma anında döner.
// Dönüş: 1 = okunabilir (oku), 0 = timeout (dış API yanıt vermedi), -1 = fiber/epoll yok
// (caller mevcut düz-blocking davranışına düşer → DEFAULT pool yolu birebir korunur).
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
    return -1;  // fiber dışı → caller blocking okur (mevcut davranış)
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
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy: " + opts.proxy; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr, /*absolute_uri=*/ via_proxy);
    if (::send(s, req.data(), req.size(), 0) < 0) {
        close(s); resp.error = "send failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    ssize_t n;
    for (;;) {
        // Fiber içindeysek: veriyi beklerken YIELD et (worker'ı bloklama). Fiber dışında
        // client_wait_readable -1 döner → doğrudan blocking recv (mevcut davranış birebir).
        int w = client_wait_readable(s, opts.timeout_ms);
        if (w == 0) { resp.error = "timeout"; break; }
        n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) { if (sink) sink->feed(buf, n); else raw.append(buf, n); continue; }
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
    if (via_proxy && !parse_proxy(opts.proxy, phost, pport)) { resp.error = "http:: invalid proxy: " + opts.proxy; return resp; }
    sock_t s = via_proxy ? tcp_connect(phost, pport, opts.timeout_ms)
                         : tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = t_conn_error ? t_conn_error : "connection failed"; return resp; }

    if (via_proxy) {
        // CONNECT tunnel to the target through the proxy, then run TLS over the tunnel.
        std::string tgt = hc_strip_crlf(url.host) + ":" + std::to_string(url.port);
        std::string creq = "CONNECT " + tgt + " HTTP/1.1\r\nHost: " + tgt + "\r\n\r\n";
        if (::send(s, creq.data(), creq.size(), 0) < 0) { close(s); resp.error = "proxy CONNECT send failed"; return resp; }
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
    SSL_set_tlsext_host_name(ssl, url.host.c_str());

    // SNI + hostname verification
    SSL_set1_host(ssl, url.host.c_str());

    if (SSL_connect(ssl) != 1) {
        ERR_clear_error();
        SSL_free(ssl); close(s);
        resp.error = "TLS handshake failed";
        return resp;
    }

    std::string req = build_request(method, url, body, hdrs, /*want_gzip=*/ on_chunk == nullptr);
    if (SSL_write(ssl, req.data(), (int)req.size()) <= 0) {
        SSL_free(ssl); close(s);
        resp.error = "SSL_write failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    int n;
    for (;;) {
        // Fiber-aware yield: SSL_pending>0 ise OpenSSL'in tamponunda çözülmüş bayt var →
        // fd okunabilir olmayabilir, BEKLEME (yoksa kilitlenir). Tampon boşsa fd okunabilir
        // olana dek yield et. Soket blocking kaldığı için SSL_read yield sonrası okur;
        // SO_RCVTIMEO ile sınırlı. Fiber dışında client_wait_readable -1 → hemen SSL_read.
        if (SSL_pending(ssl) == 0) {
            int w = client_wait_readable(s, opts.timeout_ms);
            if (w == 0) { resp.error = "timeout"; break; }
        }
        n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) { if (sink) sink->feed(buf, n); else raw.append(buf, n); continue; }
        if (n < 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;  // devam et → yield
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

    std::string authority = rest.substr(0, rest.find('/'));      // host[:port]
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
static std::string url_origin(const std::string& u) {
    std::string scheme, rest;
    if      (u.rfind("https://", 0) == 0) { scheme = "https"; rest = u.substr(8); }
    else if (u.rfind("http://",  0) == 0) { scheme = "http";  rest = u.substr(7); }
    else return u;
    std::string authority = rest.substr(0, rest.find('/'));
    for (char& c : authority) c = (char)std::tolower((unsigned char)c);
    return scheme + "://" + authority;
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

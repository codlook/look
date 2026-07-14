// http_client.cpp — Zero-dependency HTTP/HTTPS client
// Linux: OpenSSL (system libssl)
// Windows: Schannel (built-in, WinSSL)

#include "look/http_client.h"
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#ifndef _WIN32
#  include <sys/stat.h>
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

// Tek noktada IPv4 private/özel-blok kontrolü — hem ham v4 hem IPv6'ya gömülü
// v4 formları (v4-mapped, NAT64, 6to4, v4-compat) için ORTAK. Aksi halde
// gömülü-v4 formları v4 blocklist'ini atlar (IPv6-only/NAT64 ağlarında gerçek).
static bool is_private_v4(uint32_t ip) {
    if ((ip >> 24) == 127) return true;             // 127/8 loopback
    if ((ip >> 24) == 10)  return true;             // 10/8
    if ((ip >> 20) == (172*16 + 1)) return true;    // 172.16–31/12
    if ((ip >> 16) == (192*256 + 168)) return true; // 192.168/16
    if ((ip >> 16) == (169*256 + 254)) return true; // 169.254/16 (cloud metadata)
    if ((ip >> 22) == (100*4 + 1))     return true; // 100.64/10 CGNAT
    if ((ip >> 24) == 0)   return true;             // 0/8
    if ((ip >> 28) == 0xE) return true;             // 224/4 multicast
    if ((ip >> 28) == 0xF) return true;             // 240/4 reserved (255.255.255.255 dahil)
    return false;
}
static inline uint32_t rd32(const uint8_t* b) {
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}

static bool is_ssrf_blocked(const struct addrinfo* res) {
    // LOOK_ALLOW_SSRF=1 ile devre dışı bırakılabilir (iç ağ test ortamı için)
    static const bool allow = (std::getenv("LOOK_ALLOW_SSRF") != nullptr &&
                               std::string(std::getenv("LOOK_ALLOW_SSRF")) == "1");
    if (allow) return false;

    for (const struct addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            uint32_t ip = ntohl(((struct sockaddr_in*)p->ai_addr)->sin_addr.s_addr);
            if (is_private_v4(ip)) return true;
        } else if (p->ai_family == AF_INET6) {
            const uint8_t* b = ((struct sockaddr_in6*)p->ai_addr)->sin6_addr.s6_addr;
            // ::1 loopback
            bool is_lo = true;
            for (int i = 0; i < 15; ++i) if (b[i] != 0) { is_lo = false; break; }
            if (is_lo && b[15] == 1) return true;
            // ::/128 unspecified
            if (is_lo && b[15] == 0) return true;
            // fc00::/7 unique-local, fe80::/10 link-local
            if ((b[0] & 0xFE) == 0xFC) return true;
            if ((b[0] == 0xFE) && ((b[1] & 0xC0) == 0x80)) return true;

            // Gömülü-IPv4 formları — hepsi v4 blocklist'ine karşı kontrol edilmeli:
            bool hi80_zero = true;
            for (int i = 0; i < 10; ++i) if (b[i]) { hi80_zero = false; break; }
            // ::ffff:a.b.c.d — IPv4-mapped
            if (hi80_zero && b[10]==0xFF && b[11]==0xFF && is_private_v4(rd32(b+12))) return true;
            // ::a.b.c.d — IPv4-compatible (deprecated ama defense-in-depth)
            if (hi80_zero && b[10]==0 && b[11]==0 && (b[12]|b[13]|b[14]|b[15]) &&
                is_private_v4(rd32(b+12))) return true;
            // 64:ff9b::/96 — NAT64 (IPv6-only/cloud ağlarda gerçek bypass)
            if (b[0]==0 && b[1]==0x64 && b[2]==0xFF && b[3]==0x9B &&
                !b[4]&&!b[5]&&!b[6]&&!b[7]&&!b[8]&&!b[9]&&!b[10]&&!b[11] &&
                is_private_v4(rd32(b+12))) return true;
            // 2002::/16 — 6to4 (gömülü v4: b[2..5])
            if (b[0]==0x20 && b[1]==0x02 && is_private_v4(rd32(b+2))) return true;
        }
    }
    return false;
}

// ── URL parser ────────────────────────────────────────────────────────────────

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl r;
    std::string s = url;

    if (s.substr(0, 8) == "https://") { r.tls = true;  s = s.substr(8); r.port = 443; }
    else if (s.substr(0, 7) == "http://") { r.tls = false; s = s.substr(7); r.port = 80; }
    else throw std::runtime_error("http:: Desteklenmeyen URL şeması: " + url);

    auto path_pos = s.find('/');
    std::string host_port = (path_pos != std::string::npos) ? s.substr(0, path_pos) : s;
    r.path = (path_pos != std::string::npos) ? s.substr(path_pos) : "/";

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

static std::string build_request(const std::string& method,
                                  const ParsedUrl& url,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& extra_headers)
{
    std::ostringstream req;
    req << hc_strip_crlf(method) << " " << hc_strip_crlf(url.path) << " HTTP/1.1\r\n";
    req << "Host: " << hc_strip_crlf(url.host);
    if ((url.tls && url.port != 443) || (!url.tls && url.port != 80))
        req << ":" << url.port;
    req << "\r\n";
    req << "User-Agent: LOOK/0.19\r\n";
    req << "Connection: close\r\n";
    req << "Accept: */*\r\n";

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

// ── Response parser ───────────────────────────────────────────────────────────

static HttpResponse parse_response(const std::string& raw) {
    HttpResponse resp;
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
    HttpResponse resp;
    ChunkedDecoder chunked;

    explicit StreamSink(const HttpChunkCallback& c) : cb(c) {}

    void feed(const char* data, size_t n) {
        if (headers_done) { emit(std::string(data, n)); return; }
        head.append(data, n);
        size_t he = head.find("\r\n\r\n");
        if (he == std::string::npos) return;                   // basliklar henuz tam degil
        headers_done = true;
        // Baslik blogunu (bos govdeyle) parse_response ile coz — status + headers.
        HttpResponse hp = parse_response(head.substr(0, he + 4));
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

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) return INVALID;

    if (is_ssrf_blocked(res)) { freeaddrinfo(res); return INVALID; }

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

static HttpResponse do_plain(const std::string& method, const ParsedUrl& url,
                              const std::string& body,
                              const std::map<std::string, std::string>& hdrs,
                              const HttpOptions& opts,
                              const HttpChunkCallback* on_chunk = nullptr)
{
    HttpResponse resp;
    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs);
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

static HttpResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts,
                            const HttpChunkCallback* on_chunk = nullptr)
{
    HttpResponse resp;
    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

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
            // Check if Schannel has extra data from in_bufs[1]
            handshake_buf.clear();
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                handshake_buf.append((char*)in_bufs[1].pvBuffer, in_bufs[1].cbBuffer);
            }
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
        std::string req = build_request(method, url, body, hdrs);
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

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) return INVALID;

    if (is_ssrf_blocked(res)) { freeaddrinfo(res); return INVALID; }

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

static HttpResponse do_plain(const std::string& method, const ParsedUrl& url,
                              const std::string& body,
                              const std::map<std::string, std::string>& hdrs,
                              const HttpOptions& opts,
                              const HttpChunkCallback* on_chunk = nullptr)
{
    HttpResponse resp;
    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs);
    if (::send(s, req.data(), req.size(), 0) < 0) {
        close(s); resp.error = "send failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (sink) sink->feed(buf, n); else raw.append(buf, n);
    }
    if (n < 0) resp.error = "timeout";
    close(s);
    if (!resp.error.empty()) return resp;
    if (sink) { sink->finalize(); return sink->resp; }
    return parse_response(raw);
}

static HttpResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts,
                            const HttpChunkCallback* on_chunk = nullptr)
{
    HttpResponse resp;
    SSL_CTX* ctx = get_ssl_ctx();
    if (!ctx) { resp.error = "SSL_CTX init failed"; return resp; }

    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

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

    std::string req = build_request(method, url, body, hdrs);
    if (SSL_write(ssl, req.data(), (int)req.size()) <= 0) {
        SSL_free(ssl); close(s);
        resp.error = "SSL_write failed"; return resp;
    }

    std::unique_ptr<StreamSink> sink;
    if (on_chunk) sink = std::make_unique<StreamSink>(*on_chunk);
    std::string raw;
    char buf[8192];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
        if (sink) sink->feed(buf, n); else raw.append(buf, n);
    }
    if (n < 0) {
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_SYSCALL) resp.error = "timeout";
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

HttpResponse http_request(const std::string& method,
                           const std::string& url_str,
                           const std::string& body,
                           const std::map<std::string, std::string>& req_headers,
                           const HttpOptions& opts)
{
    HttpResponse resp;
    ParsedUrl url;
    try { url = parse_url(url_str); }
    catch (std::exception& e) { resp.error = e.what(); return resp; }

    try {
        if (url.tls) return do_tls(method, url, body, req_headers, opts);
        else         return do_plain(method, url, body, req_headers, opts);
    } catch (std::exception& e) {
        resp.error = e.what();
        return resp;
    }
}

HttpResponse http_request_stream(const std::string& method,
                                  const std::string& url_str,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& req_headers,
                                  const HttpOptions& opts,
                                  const HttpChunkCallback& on_chunk)
{
    HttpResponse resp;
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

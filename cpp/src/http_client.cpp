// http_client.cpp — Zero-dependency HTTP/HTTPS client
// Linux: OpenSSL (system libssl)
// Windows: Schannel (built-in, WinSSL)

#include "look/http_client.h"
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>

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
        r.port = std::stoi(host_port.substr(colon + 1));
    } else {
        r.host = host_port;
    }
    return r;
}

// ── Request builder ───────────────────────────────────────────────────────────

static std::string build_request(const std::string& method,
                                  const ParsedUrl& url,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& extra_headers)
{
    std::ostringstream req;
    req << method << " " << url.path << " HTTP/1.1\r\n";
    req << "Host: " << url.host;
    if ((url.tls && url.port != 443) || (!url.tls && url.port != 80))
        req << ":" << url.port;
    req << "\r\n";
    req << "User-Agent: LOOK/0.19\r\n";
    req << "Connection: close\r\n";
    req << "Accept: */*\r\n";

    for (auto& [k, v] : extra_headers)
        req << k << ": " << v << "\r\n";

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
                              const HttpOptions& opts)
{
    HttpResponse resp;
    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs);
    if (send(s, req.c_str(), (int)req.size(), 0) == SOCKET_ERROR) {
        closesocket(s); resp.error = "send failed"; return resp;
    }

    std::string raw;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) resp.error = "timeout";
        else resp.error = "recv failed";
        closesocket(s);
        return resp;
    }
    closesocket(s);
    return parse_response(raw);
}

static HttpResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts)
{
    HttpResponse resp;
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
                    if (dec_bufs[i].BufferType == SECBUFFER_DATA && dec_bufs[i].pvBuffer)
                        decrypted.append((char*)dec_bufs[i].pvBuffer, dec_bufs[i].cbBuffer);
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
        resp = parse_response(decrypted);
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
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);
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
                              const HttpOptions& opts)
{
    HttpResponse resp;
    sock_t s = tcp_connect(url.host, url.port, opts.timeout_ms);
    if (s == INVALID) { resp.error = "connection failed"; return resp; }

    std::string req = build_request(method, url, body, hdrs);
    if (::send(s, req.data(), req.size(), 0) < 0) {
        close(s); resp.error = "send failed"; return resp;
    }

    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    if (n < 0) resp.error = "timeout";
    close(s);
    if (!resp.error.empty()) return resp;
    return parse_response(raw);
}

static HttpResponse do_tls(const std::string& method, const ParsedUrl& url,
                            const std::string& body,
                            const std::map<std::string, std::string>& hdrs,
                            const HttpOptions& opts)
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

    std::string raw;
    char buf[8192];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) raw.append(buf, n);
    if (n < 0) {
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_SYSCALL) resp.error = "timeout";
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(s);
    if (!resp.error.empty()) return resp;
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

} // namespace look

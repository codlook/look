#include "look/resp_client.h"
#include "look/db_dsn.h"   // redis_resolve_tls (saf TLS-karar dikişi)
#include <sstream>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <vector>

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
#endif

namespace look {

#ifndef _WIN32
// ── Redis TLS: client SSL_CTX (mysql_client ile aynı kalıp) ──────────────────
// rediss:// / ?tls=1 → şifreleme (VERIFY_NONE, self-signed dostu); ?tls=verify →
// SSL_VERIFY_PEER + hostname (MITM'e karşı). Managed Redis (Upstash/ElastiCache/Redis
// Cloud) TLS ZORUNLU tutar — eski stub-throw hepsini blokluyordu.
static SSL_CTX* redis_ssl_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c) { SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr); SSL_CTX_set_default_verify_paths(c); }
        return c;
    }();
    return ctx;
}
#endif

// ── URL parser ───────────────────────────────────────────────────────────────
// redis://[:password@]host[:port][/db]
// rediss://...   (TLS — socket kurulduktan sonra OpenSSL handshake; şimdilik stub)

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
    // ESKİ HATA: substr(0,8)=="rediss://" — "rediss://" 9 karakter → 8-karakter substr ASLA
    // eşleşmezdi (rediss:// TLS'i hiç tetiklenmiyordu; stub-throw olduğu için gizli kaldı).
    // rfind(...,0)==0 = doğru "başlıyor mu" kontrolü.
    if (url.rfind("rediss://", 0) == 0) { tls = true; rest = url.substr(9); }
    else if (url.rfind("redis://", 0) == 0)             rest = url.substr(8);
    else                                                 rest = url;

    // ?tls=verify / ?ssl=verify → şifreleme + sertifika/hostname doğrulaması (MITM'e karşı).
    // ?tls=1 → yalnız şifreleme. Sorgu dizisini rest'ten ayıkla.
    { auto q = rest.find('?');
      std::string query;
      if (q != std::string::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
      }
      // TLS-karar look/db_dsn.h'de (saf, tablo-test edilebilir).
      look::redis_resolve_tls(query, tls, tls, tls_verify); }

    // password: :pass@
    auto at = rest.rfind('@');
    if (at != std::string::npos) {
        auto colon = rest.find(':');
        if (colon != std::string::npos && colon < at)
            pass = rest.substr(colon + 1, at - colon - 1);
        rest = rest.substr(at + 1);
    }

    // /db
    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        try { db = std::stoi(rest.substr(slash + 1)); } catch (...) {}
        rest = rest.substr(0, slash);
    }

    // host:port
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        try { port = std::stoi(rest.substr(colon + 1)); } catch (...) {}
    } else {
        host = rest;
    }
    if (host.empty()) host = "127.0.0.1";
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

RespClient::RespClient(const std::string& url) {
    parse_url(url, host_, port_, pass_, db_, tls_, tls_verify_);
    connect();
}

RespClient::~RespClient() {
#ifndef _WIN32
    if (ssl_) { SSL_shutdown((SSL*)ssl_); SSL_free((SSL*)ssl_); ssl_ = nullptr; }
#endif
    if (fd_ >= 0) { CLOSESOCKET(fd_); fd_ = -1; }
}

// ── TCP connect ──────────────────────────────────────────────────────────────

void RespClient::connect() {
#ifndef _WIN32
    if (ssl_) { SSL_shutdown((SSL*)ssl_); SSL_free((SSL*)ssl_); ssl_ = nullptr; }
#endif
    if (fd_ >= 0) { CLOSESOCKET(fd_); fd_ = -1; }

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0)
        throw std::runtime_error("Redis: cannot resolve " + host_);

    int sock = -1;
    for (auto* r = res; r; r = r->ai_next) {
        sock = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (sock < 0) continue;
        if (::connect(sock, r->ai_addr, (int)r->ai_addrlen) == 0) break;
        CLOSESOCKET(sock); sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) throw std::runtime_error("Redis: cannot connect to " + host_ + ":" + std::to_string(port_));
    fd_ = sock;

    // Nagle algoritmasını kapat — küçük paketlerde 40ms gecikmeyi önler
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
        // TLS: TCP kurulduktan hemen sonra SSL handshake (MySQL'in aksine mid-stream
        // SSLRequest YOK — Redis TLS bağlantı başında). ssl_ set edilince tüm I/O SSL'den.
        SSL_CTX* ctx = redis_ssl_ctx();
        if (!ctx) throw std::runtime_error("Redis: could not create SSL_CTX (TLS)");
        SSL* s = SSL_new(ctx);
        if (!s) throw std::runtime_error("Redis: SSL_new failed (TLS)");
        SSL_set_fd(s, fd_);
        SSL_set_tlsext_host_name(s, host_.c_str());   // SNI
        if (tls_verify_) {
            // ?tls=verify → sertifika + hostname doğrula (MITM'e karşı). set1_host dönüşü
            // KONTROL EDİLİR (başarısızsa hostname kontrolü sessizce kapanmasın — bkz mysql_client).
            SSL_set_verify(s, SSL_VERIFY_PEER, nullptr);
            if (SSL_set1_host(s, host_.c_str()) != 1) {
                SSL_free(s);
                throw std::runtime_error("Redis: could not set up TLS hostname verification (verify)");
            }
        }
        if (SSL_connect(s) != 1) {
            unsigned long e = ERR_get_error();
            char eb[256]; ERR_error_string_n(e, eb, sizeof(eb));
            SSL_free(s);
            throw std::runtime_error(std::string("Redis: TLS handshake failed: ") + eb);
        }
        ssl_ = s;   // bundan sonra send_command/read_* SSL üstünden
#else
        throw std::runtime_error("Redis TLS (rediss://) bu yapida desteklenmiyor "
                                 "(Windows yapisi OpenSSL'siz derlenir). Linux yapisini kullanin.");
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

    const char* p = buf.data();
    size_t rem = buf.size();
    while (rem > 0) {
        int sent;
#ifndef _WIN32
        if (ssl_) sent = SSL_write((SSL*)ssl_, p, (int)rem);
        else
#endif
            sent = (int)::send(fd_, p, (int)rem, 0);
        if (sent <= 0) throw std::runtime_error("Redis: send error");
        p += sent; rem -= sent;
    }
}

// ── RESP2 reader ─────────────────────────────────────────────────────────────

std::string RespClient::read_line() {
    // RESP2 kontrol satırları kısadır (+OK, :123, $<len>). Kötü niyetli/bozuk
    // sunucu '\n' göndermezse sınırsız büyümeyi (OOM) önlemek için üst sınır.
    static constexpr size_t RESP_MAX_LINE = 64 * 1024;
    std::string line;
    char c;
    while (true) {
        int n;
#ifndef _WIN32
        if (ssl_) n = SSL_read((SSL*)ssl_, &c, 1);
        else
#endif
            n = (int)::recv(fd_, &c, 1, 0);
        if (n <= 0) throw std::runtime_error("Redis: connection closed");
        if (c == '\r') continue;
        if (c == '\n') break;
        line += c;
        if (line.size() > RESP_MAX_LINE)
            throw std::runtime_error("Redis: line limit exceeded");
    }
    return line;
}

// Redis bulk yanıt için üst sınır — kötü niyetli/bozuk sunucu $2000000000
// döndürüp sınırsız allocation (OOM DoS) tetiklemesin. LOOK_REDIS_MAX_BULK
// (byte) ile ayarlanır, varsayılan 64 MB.
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
    // Sınır kontrolü int'e DARALTMADAN önce — $5000000000 gibi >INT_MAX değer
    // (int)len ile negatife taşıp sessizce null dönüp socket'i desync ederdi.
    if ((size_t)len > resp_max_bulk())
        throw std::runtime_error("Redis: bulk response limit exceeded (LOOK_REDIS_MAX_BULK)");
    std::string buf((size_t)len, '\0');
    size_t got = 0, need = (size_t)len;
    while (got < need) {
        int n;
#ifndef _WIN32
        if (ssl_) n = SSL_read((SSL*)ssl_, &buf[got], (int)(need - got));
        else
#endif
            n = (int)::recv(fd_, &buf[got], need - got, 0);
        if (n <= 0) throw std::runtime_error("Redis: connection closed reading bulk");
        got += (size_t)n;
    }
    read_line();  // trailing \r\n
    return buf;
}

std::string RespClient::read_response(int depth) {
    // Derin iç içe dizi (*1\r\n*1\r\n...) recursive parse'ı stack'i taşırabilir —
    // kötü niyetli/bozuk sunucuya karşı derinlik sınırı.
    static constexpr int RESP_MAX_DEPTH = 64;
    if (depth > RESP_MAX_DEPTH)
        throw std::runtime_error("Redis: response nested too deep (array)");
    auto line = read_line();
    if (line.empty()) throw std::runtime_error("Redis: empty response");

    char type = line[0];
    std::string payload = line.substr(1);

    switch (type) {
        case '+': return payload;                        // Simple string
        case '-': throw std::runtime_error("Redis: " + payload);  // Error
        case ':': return payload;                        // Integer (as string)
        case '$': {                                      // Bulk string
            long len = 0;
            try { len = std::stol(payload); }            // bozuk uzunluk → protokol hatası
            catch (...) { throw std::runtime_error("Redis: invalid bulk length"); }
            if (len == -1) return "";                    // nil
            return read_bulk(len);
        }
        case '*': {                                      // Array — flatten for our use
            long count = 0;
            try { count = std::stol(payload); }
            catch (...) { throw std::runtime_error("Redis: invalid array count"); }
            if (count <= 0) return "";
            // Sunucu-verili count sanity cap — kötü niyetli/MITM sunucu (TLS yok)
            // `*2000000000` verirse döngü kaynak tüketir. Session subset'i küçük
            // dizilerle çalışır; makul üst sınır. (MySQL col_count / RESP_MAX_DEPTH
            // ile tutarlı.)
            static constexpr long RESP_MAX_ARRAY = 1024 * 1024;
            if (count > RESP_MAX_ARRAY)
                throw std::runtime_error("Redis: array element count limit exceeded");
            std::string first;
            for (int i = 0; i < count; i++) {
                auto v = read_response(depth + 1);
                if (i == 0) first = v;
            }
            return first;
        }
        default:
            throw std::runtime_error("Redis: unknown response type " + std::string(1, type));
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

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

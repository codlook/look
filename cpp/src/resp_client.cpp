#include "look/resp_client.h"
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

namespace look {

// ── URL parser ───────────────────────────────────────────────────────────────
// redis://[:password@]host[:port][/db]
// rediss://...   (TLS — socket kurulduktan sonra OpenSSL handshake; şimdilik stub)

static void parse_url(const std::string& url,
                      std::string& host, int& port,
                      std::string& pass, int& db, bool& tls)
{
    host = "127.0.0.1";
    port = 6379;
    pass = "";
    db   = 0;
    tls  = false;

    std::string rest;
    if (url.substr(0, 8) == "rediss://") { tls = true;  rest = url.substr(8); }
    else if (url.substr(0, 8) == "redis://")             rest = url.substr(8);
    else                                                  rest = url;

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
    parse_url(url, host_, port_, pass_, db_, tls_);
    connect();
}

RespClient::~RespClient() {
    if (fd_ >= 0) { CLOSESOCKET(fd_); fd_ = -1; }
}

// ── TCP connect ──────────────────────────────────────────────────────────────

void RespClient::connect() {
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

    if (tls_)
        throw std::runtime_error("Redis TLS (rediss://) requires OpenSSL — not compiled in this build");

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
        int sent = (int)::send(fd_, p, (int)rem, 0);
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
        int n = (int)::recv(fd_, &c, 1, 0);
        if (n <= 0) throw std::runtime_error("Redis: connection closed");
        if (c == '\r') continue;
        if (c == '\n') break;
        line += c;
        if (line.size() > RESP_MAX_LINE)
            throw std::runtime_error("Redis: satır sınırı aşıldı");
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
        throw std::runtime_error("Redis: bulk yanıt sınırı aşıldı (LOOK_REDIS_MAX_BULK)");
    std::string buf((size_t)len, '\0');
    size_t got = 0, need = (size_t)len;
    while (got < need) {
        int n = (int)::recv(fd_, &buf[got], need - got, 0);
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
        throw std::runtime_error("Redis: yanıt çok derin iç içe (dizi)");
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
            catch (...) { throw std::runtime_error("Redis: geçersiz bulk uzunluğu"); }
            if (len == -1) return "";                    // nil
            return read_bulk(len);
        }
        case '*': {                                      // Array — flatten for our use
            long count = 0;
            try { count = std::stol(payload); }
            catch (...) { throw std::runtime_error("Redis: geçersiz dizi sayısı"); }
            if (count <= 0) return "";
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

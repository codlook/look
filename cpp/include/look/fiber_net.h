#pragma once
// Ağ istemcilerinin (MySQL, PostgreSQL, Redis, SMTP, HTTP) bağlantı kurma, DNS, TLS el sıkışması
// ve gönderme adımları için ortak yol. fiber_io.h okumayı çözmüştü; bu adımlar hâlâ İŞ
// PARÇACIĞINI tutuyordu: getaddrinfo yavaş bir çözücüde saniyelerce, engelleyen connect uzak
// sunucu SYN'e yanıt vermezse dakikalarca, SSL_connect her tur yolculuğunda, dolu gönderme
// tamponunda send/SSL_write karşı taraf okuyana dek. Fiber içinde bunların hepsinde yalnız fiber
// bekler; fiber dışında (komut satırı, kurulum, zamanlayıcı iş parçacıkları) eski engelleyen
// davranış korunur (SO_RCVTIMEO/SO_SNDTIMEO ile sınırlı).
// Yalnız POSIX: Windows'ta fiber yok, istemcilerin _WIN32 dalları kendi yolunu kullanır.
#ifndef _WIN32
#include "look/fiber_io.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/ssl.h>

namespace look {

inline bool in_fiber() { return Fiber::current() && get_thread_scheduler(); }

// DNS çözümleme. Sayısal adres (127.0.0.1, ::1) çözücüye gitmez → doğrudan. Ad çözümlemesi
// fiber içinde ortak iş havuzunda (fiber_offload) koşar: çözücü yanıt vermese de iş parçacığı
// öteki isteklere bakar.
inline int fiber_getaddrinfo(const char* node, const char* service,
                             const struct addrinfo* hints, struct addrinfo** res) {
    unsigned char tmp[sizeof(struct in6_addr)];
    if (!node || inet_pton(AF_INET, node, tmp) == 1 || inet_pton(AF_INET6, node, tmp) == 1)
        return ::getaddrinfo(node, service, hints, res);   // fiber-safe: sayısal, çözücü yok
    return fiber_offload([&] { return ::getaddrinfo(node, service, hints, res); });   // fiber-safe: havuzda
}

// Bağlan; en çok timeout_ms bekle (<= 0 → çekirdeğin kendi sınırı). Soket geçici engellemesiz:
// fiber içinde yalnız fiber bekler. Dönüş 0 = bağlandı; -1 = hata (errno; süre doldu → ETIMEDOUT).
// Soketin önceki kipi (engelleyen) geri yüklenir.
inline int fiber_connect(int fd, const struct sockaddr* addr, socklen_t len, int timeout_ms) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, addr, len);   // fiber-safe: engellemesiz
    if (rc != 0 && errno == EINPROGRESS) {
        int w = fiber_wait_writable(fd, timeout_ms);
        if (w < 0) {   // fiber yok → poll ile bekle (blocking-ok: fiber yok)
            struct pollfd p{}; p.fd = fd; p.events = POLLOUT;
            w = ::poll(&p, 1, timeout_ms > 0 ? timeout_ms : -1);   // blocking-ok: fiber yok
        }
        if (w == 0) { errno = ETIMEDOUT; rc = -1; }
        else if (w > 0) {
            int err = 0; socklen_t el = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { errno = err; rc = -1; } else rc = 0;
        }
    }
    int e = errno;
    ::fcntl(fd, F_SETFL, fl);
    errno = e;
    return rc;
}

// TLS el sıkışması. Dönüş 1 = tamam; 0 = süre doldu; -1 = TLS hatası (OpenSSL hata kuyruğu dolu).
// Fiber dışında eski engelleyen SSL_connect (SO_RCVTIMEO/SO_SNDTIMEO ile sınırlı).
inline int fiber_ssl_connect(SSL* s, int fd, int timeout_ms) {
    if (!in_fiber()) return SSL_connect(s) == 1 ? 1 : -1;   // blocking-ok: fiber yok
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int out;
    for (;;) {
        int r = SSL_connect(s);   // fiber-safe: engellemesiz
        if (r == 1) { out = 1; break; }
        int e = SSL_get_error(s, r), w;
        if (e == SSL_ERROR_WANT_READ)       w = fiber_wait_readable(fd, timeout_ms);
        else if (e == SSL_ERROR_WANT_WRITE) w = fiber_wait_writable(fd, timeout_ms);
        else { out = -1; break; }
        if (w <= 0) { out = 0; break; }
    }
    ::fcntl(fd, F_SETFL, fl);
    return out;
}

// Tamamını gönder (kısmi yazmada kalanı da). Dönüş true = hepsi gitti; false = hata / süre doldu
// (errno ETIMEDOUT). Fiber içinde: düz sokette MSG_DONTWAIT, TLS'te soket geçici engellemesiz —
// tampon doluysa yalnız fiber bekler. SIGPIPE yok (kapalı sokette EPIPE).
inline bool fiber_send_all(int fd, SSL* ssl, const void* buf, size_t len, int timeout_ms) {
    const char* p = static_cast<const char*>(buf);
    const bool fib = in_fiber();
    int fl = -1;
    if (fib && ssl) { fl = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
    bool ok = true;
    while (len > 0) {
        size_t chunk = len > (size_t)INT_MAX ? (size_t)INT_MAX : len;
        int w = 1;   // 1 = yazıldı, 2 = yazılabilir bekle, 3 = okunabilir bekle
        ssize_t n;
        if (ssl) {
            n = SSL_write(ssl, p, (int)chunk);   // fiber-safe: fiber içinde soket engellemesiz
            if (n <= 0) {
                int e = SSL_get_error(ssl, (int)n);
                if (fib && e == SSL_ERROR_WANT_WRITE) w = 2;
                else if (fib && e == SSL_ERROR_WANT_READ) w = 3;
                else { ok = false; break; }
            }
        } else {
            n = ::send(fd, p, chunk, MSG_NOSIGNAL | (fib ? MSG_DONTWAIT : 0));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && fib && (errno == EAGAIN || errno == EWOULDBLOCK)) w = 2;
            else if (n <= 0) { ok = false; break; }
        }
        if (w == 1) { p += n; len -= (size_t)n; continue; }
        int r = (w == 2) ? fiber_wait_writable(fd, timeout_ms) : fiber_wait_readable(fd, timeout_ms);
        if (r <= 0) { if (r == 0) errno = ETIMEDOUT; ok = false; break; }
    }
    if (fl != -1) { int e = errno; ::fcntl(fd, F_SETFL, fl); errno = e; }
    return ok;
}

} // namespace look
#else  // _WIN32: fiber yok — çağıran kod iki platformda aynı adı kullansın diye
#include <winsock2.h>
#include <ws2tcpip.h>
namespace look {
inline int fiber_getaddrinfo(const char* node, const char* service,
                             const struct addrinfo* hints, struct addrinfo** res) {
    return ::getaddrinfo(node, service, hints, res);
}
} // namespace look
#endif // !_WIN32

#include <string>
namespace look {
#ifdef _WIN32
using dial_sock_t = SOCKET;
constexpr dial_sock_t DIAL_INVALID = INVALID_SOCKET;
#else
using dial_sock_t = int;
constexpr dial_sock_t DIAL_INVALID = -1;
#endif

// Go net.Dial modeli: adı AF_UNSPEC ile çözümle, dönen adresleri (IPv4 ve IPv6) sırayla dene, ilk
// bağlananı döndür. Eskiden MySQL/PG istemcileri yalnız AF_INET çözüyordu → "[::1]:5432" ya da
// yalnız AAAA kaydı olan bir sunucuya hiç bağlanılamıyordu. Her deneme en çok timeout_ms.
// Dönüş: bağlı (engelleyen kipte) soket ya da DIAL_INVALID; resolved=false → ad çözülemedi;
// timed_out → en az bir deneme süre aşımına uğradı.
inline dial_sock_t tcp_dial(const std::string& host, int port, int timeout_ms,
                            bool& resolved, bool& timed_out) {
    resolved = false; timed_out = false;
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string ps = std::to_string(port);
    if (fiber_getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) return DIAL_INVALID;
    resolved = true;
    dial_sock_t s = DIAL_INVALID;
    for (auto* r = res; r; r = r->ai_next) {
        s = (dial_sock_t)::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (s == DIAL_INVALID) continue;
#ifndef _WIN32
        if (fiber_connect(s, r->ai_addr, (socklen_t)r->ai_addrlen, timeout_ms) == 0) break;
        if (errno == ETIMEDOUT) timed_out = true;
        ::close(s);
#else
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        ::connect(s, r->ai_addr, (int)r->ai_addrlen);
        fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
        fd_set efds; FD_ZERO(&efds); FD_SET(s, &efds);
        struct timeval tv; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = ::select(0, nullptr, &wfds, &efds, timeout_ms > 0 ? &tv : nullptr);
        int err = 0; int el = sizeof(err);
        if (sel > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (sel > 0 && err == 0 && FD_ISSET(s, &wfds)) { u_long b = 0; ioctlsocket(s, FIONBIO, &b); break; }
        if (sel == 0) timed_out = true;
        ::closesocket(s);
#endif
        s = DIAL_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

// Hata metninde adres: IPv6 köşeli parantezde ("[::1]:5432"), Go net.JoinHostPort gibi.
inline std::string join_host_port(const std::string& host, int port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
} // namespace look

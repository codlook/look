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

#pragma once
// A common path for the network clients' (MySQL, PostgreSQL, Redis, SMTP, HTTP) connect, DNS, TLS handshake
// and send steps. fiber_io.h had solved reading; these steps still held the THREAD:
// getaddrinfo for seconds on a slow resolver, a blocking connect for minutes if the remote
// server does not answer the SYN, SSL_connect on every round trip, send/SSL_write on a full send
// buffer until the other side reads. Inside a fiber only the fiber waits in all of these;
// outside a fiber (command line, setup, timer threads) the old blocking
// behaviour is kept (bounded by SO_RCVTIMEO/SO_SNDTIMEO).
// POSIX only: Windows has no fibers, the clients' _WIN32 branches use their own path.
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

// DNS resolution. A numeric address (127.0.0.1, ::1) does not go to the resolver → direct. Name resolution
// runs in the shared job pool (fiber_offload) inside a fiber: even if the resolver does not answer the thread
// keeps serving other requests.
inline int fiber_getaddrinfo(const char* node, const char* service,
                             const struct addrinfo* hints, struct addrinfo** res) {
    unsigned char tmp[sizeof(struct in6_addr)];
    if (!node || inet_pton(AF_INET, node, tmp) == 1 || inet_pton(AF_INET6, node, tmp) == 1)
        return ::getaddrinfo(node, service, hints, res);   // fiber-safe: numeric, no resolver
    return fiber_offload([&] { return ::getaddrinfo(node, service, hints, res); });   // fiber-safe: on the pool
}

// Connect; wait at most timeout_ms (<= 0 → the kernel's own limit). The socket is temporarily non-blocking:
// inside a fiber only the fiber waits. Returns 0 = connected; -1 = error (errno; time ran out → ETIMEDOUT).
// The socket's previous (blocking) mode is restored.
inline int fiber_connect(int fd, const struct sockaddr* addr, socklen_t len, int timeout_ms) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, addr, len);   // fiber-safe: non-blocking
    if (rc != 0 && errno == EINPROGRESS) {
        int w = fiber_wait_writable(fd, timeout_ms);
        if (w < 0) {   // no fiber → wait with poll (blocking-ok: no fiber)
            struct pollfd p{}; p.fd = fd; p.events = POLLOUT;
            w = ::poll(&p, 1, timeout_ms > 0 ? timeout_ms : -1);   // blocking-ok: no fiber
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

// TLS handshake. Returns 1 = done; 0 = time ran out; -1 = TLS error (OpenSSL error queue is filled).
// Outside a fiber the old blocking SSL_connect (bounded by SO_RCVTIMEO/SO_SNDTIMEO).
inline int fiber_ssl_connect(SSL* s, int fd, int timeout_ms) {
    if (!in_fiber()) return SSL_connect(s) == 1 ? 1 : -1;   // blocking-ok: no fiber
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int out;
    for (;;) {
        int r = SSL_connect(s);   // fiber-safe: non-blocking
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

// Send everything (the remainder too after a partial write). Returns true = all went out; false = error / time ran out
// (errno ETIMEDOUT). Inside a fiber: MSG_DONTWAIT on a plain socket, on TLS the socket is temporarily non-blocking —
// if the buffer is full only the fiber waits. No SIGPIPE (EPIPE on a closed socket).
inline bool fiber_send_all(int fd, SSL* ssl, const void* buf, size_t len, int timeout_ms) {
    const char* p = static_cast<const char*>(buf);
    const bool fib = in_fiber();
    int fl = -1;
    if (fib && ssl) { fl = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
    bool ok = true;
    while (len > 0) {
        size_t chunk = len > (size_t)INT_MAX ? (size_t)INT_MAX : len;
        int w = 1;   // 1 = written, 2 = wait for writable, 3 = wait for readable
        ssize_t n;
        if (ssl) {
            n = SSL_write(ssl, p, (int)chunk);   // fiber-safe: the socket is non-blocking inside a fiber
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
#else  // _WIN32: no fiber — so that the calling code uses the same name on both platforms
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

// Go net.Dial model: resolve the name with AF_UNSPEC, try the returned addresses (IPv4 and IPv6) in order, return the first
// one that connects. MySQL/PG clients used to resolve only AF_INET → a server with "[::1]:5432" or
// only an AAAA record could never be connected to. Each attempt at most timeout_ms.
// Returns: a connected (blocking-mode) socket or DIAL_INVALID; resolved=false → the name could not be resolved;
// timed_out → at least one attempt timed out.
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

// Address in the error text: IPv6 in square brackets ("[::1]:5432"), like Go's net.JoinHostPort.
inline std::string join_host_port(const std::string& host, int port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
} // namespace look

#pragma once
// Availability of an idle waiting database socket -- WITHOUT SENDING ANYTHING.
//
// After a query's reply has been fully read the socket should not be readable. If it is readable
// or in error, the server has closed the connection (FIN/RST: wait_timeout, restart, KILL,
// pg_terminate_backend) or sent an error packet before closing (MySQL 8 wait_timeout).
// Detected BEFORE the query -> a new connection is safe, because nothing was sent (no replay).
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <poll.h>
#  include <cerrno>
#endif

namespace look {

template <class Sock>
inline bool idle_socket_unusable(Sock fd) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, 0) != 0;   // readable, error or closed
#else
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    int r;
    do { r = ::poll(&p, 1, 0); } while (r < 0 && errno == EINTR);
    return r != 0;                   // readable (data/EOF), POLLERR/POLLHUP or poll error
#endif
}

} // namespace look

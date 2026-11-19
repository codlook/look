#pragma once
// Common wait point of the database clients: while inside a fiber, until the socket becomes readable
// (or time runs out) it suspends only the fiber, NOT THE THREAD. MySQL and
// PostgreSQL use the same path -- one client's wait does not affect the other
// requests on that thread.
#include "look/fiber.h"
#ifndef _WIN32
#  include <poll.h>
#endif

namespace look {

// Return: 1 = readable, 0 = timed out, -1 = no fiber/unsupported (the caller reads by blocking).
// timeout_ms <= 0 -> unbounded.
inline int fiber_wait_readable(int fd, int timeout_ms) {
#ifdef _WIN32
    (void)fd; (void)timeout_ms;
    return -1;
#else
    Fiber* cur = Fiber::current();
    if (!cur) return -1;
    FiberScheduler* sched = get_thread_scheduler();
    if (!sched) return -1;
    // NON-WAITING poll before suspending: if the response has already arrived there is no suspend/wake-up.
    // It used to be a poll that WAITED 1 ms (assuming "suspend+wake-up ~1 ms"): on every DB wait the
    // thread could not serve any other request for 1 ms. Measured (8 workers, pool 32, MySQL 5 ms,
    // wrk): in the off-CPU profile the workers were in this poll in most samples; the DB route was ~15% behind 32 workers.
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0) return 1;
    auto sp = cur->shared_from_this_fiber();
    if (!sp) return -1;
    return sched->wait_readable_tmo(sp, fd, timeout_ms > 0 ? timeout_ms : -1);
#endif
}

// Suspend only the fiber until writable (until a non-blocking connect finishes).
// Return: 1 = writable, 0 = timed out, -1 = no fiber (the caller waits with poll).
inline int fiber_wait_writable(int fd, int timeout_ms) {
#ifdef _WIN32
    (void)fd; (void)timeout_ms;
    return -1;
#else
    Fiber* cur = Fiber::current();
    if (!cur) return -1;
    FiberScheduler* sched = get_thread_scheduler();
    if (!sched) return -1;
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, 0) > 0) return 1;
    auto sp = cur->shared_from_this_fiber();
    if (!sp) return -1;
    return sched->wait_writable_tmo(sp, fd, timeout_ms > 0 ? timeout_ms : -1);
#endif
}

} // namespace look

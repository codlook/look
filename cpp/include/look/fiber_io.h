#pragma once
// Veritabanı istemcilerinin ortak bekleme noktası: fiber içindeyken soket okunabilir olana
// (ya da süre dolana) dek İŞ PARÇACIĞINI DEĞİL yalnız fiber'ı askıya alır. MySQL ve
// PostgreSQL aynı yolu kullanır — bir istemcinin beklemesi o iş parçacığındaki öteki
// isteklere yansımaz.
#include "look/fiber.h"
#ifndef _WIN32
#  include <poll.h>
#endif

namespace look {

// Dönüş: 1 = okunabilir, 0 = süre doldu, -1 = fiber yok/desteklenmiyor (çağıran bloklayarak okur).
// timeout_ms <= 0 → süresiz.
inline int fiber_wait_readable(int fd, int timeout_ms) {
#ifdef _WIN32
    (void)fd; (void)timeout_ms;
    return -1;
#else
    Fiber* cur = Fiber::current();
    if (!cur) return -1;
    FiberScheduler* sched = get_thread_scheduler();
    if (!sched) return -1;
    // Askıya almadan önce kısa poll: localhost veritabanı yanıtı çoğu kez <1 ms'de gelir;
    // askı+epoll+uyanma zinciri (~1 ms) çoğu sorguda hiç yaşanmaz.
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 1) > 0) return 1;
    auto sp = cur->shared_from_this_fiber();
    if (!sp) return -1;
    return sched->wait_readable_tmo(sp, fd, timeout_ms > 0 ? timeout_ms : -1);
#endif
}

} // namespace look

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
    // Askıya almadan önce BEKLEMESİZ yoklama: yanıt zaten geldiyse askı/uyanma yaşanmaz.
    // Eskiden 1 ms BEKLEYEN poll vardı ("askı+uyanma ~1 ms" varsayımıyla): her DB beklemesinde
    // iş parçacığı 1 ms başka hiçbir isteğe bakamıyordu. Ölçüldü (8 işçi, havuz 32, MySQL 5 ms,
    // wrk): off-CPU profilinde işçiler çoğu örnekte bu poll'da; DB rotası 32 işçinin ~%15 gerisindeydi.
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0) return 1;
    auto sp = cur->shared_from_this_fiber();
    if (!sp) return -1;
    return sched->wait_readable_tmo(sp, fd, timeout_ms > 0 ? timeout_ms : -1);
#endif
}

// Yazılabilir olana dek (engellemesiz connect'in bitmesi) yalnız fiber'ı askıya al.
// Dönüş: 1 = yazılabilir, 0 = süre doldu, -1 = fiber yok (çağıran poll ile bekler).
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

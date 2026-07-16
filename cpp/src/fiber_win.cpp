// fiber_win.cpp — Stackful fiber implementation for Windows
//
// Uses Win32 Fiber API: CreateFiber / SwitchToFiber / DeleteFiber.
// Guard page is handled automatically by Windows stack guard mechanism;
// we also set FIBER_FLAG_FLOAT_SWITCH for correct FPU state handling.
//
// Thread model: same as fiber_posix.cpp — one thread context, N fibers.
// The thread must be converted to a fiber before switching to any fiber.

#include "look/fiber.h"
#include "look/logger.h"

#ifndef _WIN32
#  error "fiber_win.cpp must only be compiled on Windows"
#endif

#include <cassert>
#include <stdexcept>
#include <string>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace look {

// ── Platform impl ─────────────────────────────────────────────────────────────

struct Fiber::Impl {
    LPVOID fiber_handle  = nullptr;  // this fiber's Win32 fiber
    LPVOID caller_handle = nullptr;  // caller's fiber/thread handle (on resume)
};

// ── Thread-local state ────────────────────────────────────────────────────────

static thread_local Fiber*          tl_current_fiber = nullptr;
static thread_local FiberScheduler* tl_scheduler     = nullptr;

// Thread fiber handle — the thread itself must be converted to a fiber
// before SwitchToFiber can be used. We do this lazily on first resume().
static thread_local LPVOID tl_thread_fiber = nullptr;

static LPVOID ensure_thread_fiber() {
    if (!tl_thread_fiber) {
        tl_thread_fiber = ::ConvertThreadToFiberEx(nullptr,
                                                    FIBER_FLAG_FLOAT_SWITCH);
        if (!tl_thread_fiber)
            throw std::runtime_error("fiber: ConvertThreadToFiber failed");
    }
    return tl_thread_fiber;
}

Fiber* Fiber::current() noexcept { return tl_current_fiber; }

FiberScheduler* get_thread_scheduler() noexcept { return tl_scheduler; }
void            set_thread_scheduler(FiberScheduler* s) noexcept { tl_scheduler = s; }

// ── Guard page ────────────────────────────────────────────────────────────────
// Windows automatically sets up a guard page on fiber stacks.
// No manual mprotect needed.

void Fiber::install_guard_page() { /* handled by OS */ }
void Fiber::remove_guard_page()  { /* handled by OS */ }

// ── Entry point ───────────────────────────────────────────────────────────────

static VOID WINAPI fiber_trampoline(LPVOID param) noexcept {
    Fiber::fiber_entry(static_cast<Fiber*>(param));
    // After fiber_entry returns: switch back to the caller.
    // tl_current_fiber was set to nullptr by fiber_entry.
    Fiber* self = static_cast<Fiber*>(param);
    ::SwitchToFiber(self->impl_->caller_handle);
}

void Fiber::fiber_entry(Fiber* self) noexcept {
    try {
        self->fn_();
    } catch (const std::exception& ex) {
        Logger::instance().log(LogLevel::LOG_ERROR, "fiber",
            std::string("fiber panic: ") + ex.what());
    } catch (...) {
        Logger::instance().log(LogLevel::LOG_ERROR, "fiber",
            "fiber panic: unknown exception");
    }
    self->done_ = true;
    tl_current_fiber = nullptr;
}

// ── Fiber construction ────────────────────────────────────────────────────────

Fiber::Fiber(Fn fn, size_t stack_size)
    : impl_(std::make_unique<Impl>())
    , fn_(std::move(fn))
{
    impl_->fiber_handle = ::CreateFiberEx(
        stack_size,           // initial commit size
        stack_size,           // maximum stack size
        FIBER_FLAG_FLOAT_SWITCH,
        fiber_trampoline,
        this);

    if (!impl_->fiber_handle)
        throw std::runtime_error("fiber: CreateFiberEx failed, error=" +
                                  std::to_string(::GetLastError()));
}

Fiber::~Fiber() {
    if (impl_ && impl_->fiber_handle) {
        ::DeleteFiber(impl_->fiber_handle);
        impl_->fiber_handle = nullptr;
    }
}

std::shared_ptr<Fiber> Fiber::create(Fn fn, size_t stack_size) {
    std::shared_ptr<Fiber> f(new Fiber(std::move(fn), stack_size));
    f->weak_self_ = f;  // weak reference to self for I/O resume callbacks
    return f;
}

// ── Context switch ────────────────────────────────────────────────────────────

void Fiber::resume() {
    assert(!done_ && "resuming a completed fiber");

    LPVOID caller = ensure_thread_fiber();
    impl_->caller_handle = caller;

    Fiber* prev      = tl_current_fiber;
    tl_current_fiber = this;

    ::SwitchToFiber(impl_->fiber_handle);

    // Returns here when fiber calls yield() or completes
    tl_current_fiber = prev;
}

void Fiber::yield() {
    Fiber* self = tl_current_fiber;
    assert(self && "yield() called outside a fiber");

    ::SwitchToFiber(self->impl_->caller_handle);
}

// ── FiberScheduler — same logic as POSIX backend ─────────────────────────────

FiberScheduler::FiberScheduler() {
    // Windows: no epoll — epfd_ stays -1; wait_readable uses blocking recv fallback
}

FiberScheduler::~FiberScheduler() {}

void FiberScheduler::spawn(Fiber::Fn fn, size_t stack_size) {
    auto f = Fiber::create(std::move(fn), stack_size);
    std::lock_guard<std::mutex> lk(mtx_);
    ready_.push_back(std::move(f));
}

bool FiberScheduler::run_one() {
    std::shared_ptr<Fiber> f;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (ready_.empty()) return false;
        f = std::move(ready_.front());
        ready_.erase(ready_.begin());
    }

    f->sched_state_.store(Fiber::SchedState::RUNNING, std::memory_order_release);
    f->resume();

    // Yield dönüşü — fiber'ın akıbetine tek yetkili burası (posix ile aynı protokol)
    std::lock_guard<std::mutex> lk(mtx_);
    if (f->done()) {
        f->sched_state_.store(Fiber::SchedState::DONE, std::memory_order_release);
        auto wit = std::find(waiting_.begin(), waiting_.end(), f);
        if (wit != waiting_.end()) waiting_.erase(wit);
        auto pit = std::find(pool_waiters_.begin(), pool_waiters_.end(), f);
        if (pit != pool_waiters_.end()) pool_waiters_.erase(pit);
        f->pending_resume_.store(false, std::memory_order_relaxed);
        return true;
    }

    auto st = f->sched_state_.load(std::memory_order_acquire);
    if (st == Fiber::SchedState::SUSPENDING) {
        if (f->pending_resume_.exchange(false, std::memory_order_acq_rel)) {
            auto wit = std::find(waiting_.begin(), waiting_.end(), f);
            if (wit != waiting_.end()) waiting_.erase(wit);
            auto pit = std::find(pool_waiters_.begin(), pool_waiters_.end(), f);
            if (pit != pool_waiters_.end()) pool_waiters_.erase(pit);
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        } else {
            f->sched_state_.store(Fiber::SchedState::WAITING, std::memory_order_release);
        }
    } else {
        f->pending_resume_.store(false, std::memory_order_relaxed);
        f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
        ready_.push_back(std::move(f));
    }
    return true;
}

void FiberScheduler::run_until_idle() {
    while (run_one()) {}
}

bool FiberScheduler::wait_readable(std::shared_ptr<Fiber> f, int fd) {
    // Windows: no per-scheduler epoll — caller should use blocking recv.
    (void)f; (void)fd;
    return false;
}

int FiberScheduler::wait_readable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms) {
    // Windows: epoll yok — desteklenmiyor (çağıran blocking yola düşer).
    (void)f; (void)fd; (void)timeout_ms;
    return -1;
}

void FiberScheduler::run_until_complete() {
    while (true) {
        while (run_one()) {}
        std::unique_lock<std::mutex> lk(mtx_);
        if (waiting_.empty()) break;
        cv_.wait(lk, [this] { return !ready_.empty() || waiting_.empty(); });
    }
}

void FiberScheduler::suspend_current() {
    FiberScheduler* sched = tl_scheduler;
    Fiber*          cur   = Fiber::current();
    assert(cur   && "suspend_current() called outside a fiber");
    assert(sched && "no scheduler on this thread");

    auto sp = cur->shared_from_this_fiber();
    if (!sp) return;

    {
        std::lock_guard<std::mutex> lk(sched->mtx_);
        sched->waiting_.push_back(sp);
        cur->sched_state_.store(Fiber::SchedState::SUSPENDING, std::memory_order_release);
    }
    // Erken resume'u run_one() pending_resume_ ile işler — ön-kontrol yok
    Fiber::yield();
}

void FiberScheduler::suspend_current_for_pool() {
    FiberScheduler* sched = tl_scheduler;
    Fiber*          cur   = Fiber::current();
    assert(cur   && "suspend_current_for_pool() called outside a fiber");
    assert(sched && "no scheduler on this thread");

    auto sp = cur->shared_from_this_fiber();
    if (!sp) return;

    {
        std::lock_guard<std::mutex> lk(sched->mtx_);
        sched->waiting_.push_back(sp);
        sched->pool_waiters_.push_back(sp);
        cur->sched_state_.store(Fiber::SchedState::SUSPENDING, std::memory_order_release);
    }
    Fiber::yield();
}

void FiberScheduler::notify_pool_waiter() {
    std::shared_ptr<Fiber> f;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pool_waiters_.empty()) return;
        f = std::move(pool_waiters_.front());
        pool_waiters_.erase(pool_waiters_.begin());
    }
    if (f) resume_fiber(std::move(f));
}

void FiberScheduler::resume_fiber(std::shared_ptr<Fiber> f) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto st = f->sched_state_.load(std::memory_order_acquire);

    if (st == Fiber::SchedState::WAITING) {
        auto it = std::find(waiting_.begin(), waiting_.end(), f);
        if (it != waiting_.end()) waiting_.erase(it);
        if (!f->done()) {
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        }
        cv_.notify_one();
        return;
    }

    // SUSPENDING/RUNNING: ready_'ye dokunma — run_one() yield dönüşünde işler
    f->pending_resume_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void FiberScheduler::wake() {
    // Windows backend'de run_until_complete cv_ üzerinde bekler —
    // resume_fiber zaten notify ediyor, ek uyandırma gerekmez.
    cv_.notify_one();
}

size_t FiberScheduler::ready_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ready_.size();
}

size_t FiberScheduler::total_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ready_.size() + waiting_.size();
}

} // namespace look

// fiber_posix.cpp — Stackful fiber implementation for Linux / macOS
//
// Uses POSIX ucontext_t for context switching.
// Guard page installed via mprotect() to catch stack overflow early.
//
// Thread model:
//   Each OS thread has one "thread context" (not a Fiber).
//   Fiber::resume() switches from thread context → fiber context.
//   Fiber::yield()  switches from fiber context → thread context.
//
// Note: ucontext_t is marked deprecated on macOS but still works.
//   If this becomes a problem, replace with asm context switch (50 lines).

#include "look/fiber.h"
#include "look/logger.h"

#include <cassert>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

// POSIX
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <ucontext.h>
#include <unistd.h>

namespace look {

// ── Platform impl ─────────────────────────────────────────────────────────────

struct Fiber::Impl {
    ucontext_t fiber_ctx{};    // this fiber's execution context
    ucontext_t caller_ctx{};   // context to return to on yield()

    // Raw stack allocation (mmap for guard page support)
    void*  stack_base  = nullptr;  // bottom of allocation (guard page here)
    size_t stack_alloc = 0;        // total allocation size (stack + guard page)
    size_t stack_size  = 0;        // usable stack size (without guard page)
};

// ── Thread-local state ────────────────────────────────────────────────────────

static thread_local Fiber*           tl_current_fiber   = nullptr;
static thread_local FiberScheduler*  tl_scheduler       = nullptr;

Fiber* Fiber::current() noexcept { return tl_current_fiber; }

FiberScheduler* get_thread_scheduler() noexcept { return tl_scheduler; }
void            set_thread_scheduler(FiberScheduler* s) noexcept { tl_scheduler = s; }

// ── Guard page ────────────────────────────────────────────────────────────────

void Fiber::install_guard_page() {
    if (!impl_ || !impl_->stack_base) return;

    // Mark the bottom page (lowest address) as inaccessible.
    // Stack grows downward on x86_64, so overflow hits this page first.
    if (::mprotect(impl_->stack_base, GUARD_SIZE, PROT_NONE) != 0) {
        // Non-fatal: guard page is a safety net, not required for correctness.
        Logger::instance().log(LogLevel::LOG_WARN, "fiber",
            "mprotect guard page failed: " + std::string(std::strerror(errno)));
    }
}

void Fiber::remove_guard_page() {
    if (!impl_ || !impl_->stack_base) return;
    // Restore permissions so munmap can release the memory cleanly.
    ::mprotect(impl_->stack_base, GUARD_SIZE, PROT_READ | PROT_WRITE);
}

// ── Entry point ───────────────────────────────────────────────────────────────
//
// Called once when a fiber starts for the first time.
// We can't pass args through makecontext portably (only int args on some ABIs),
// so we store the Fiber* in the ucontext user data via a static dispatch.
//
// makecontext accepts only int arguments — pass pointer as two ints (high/low).

void fiber_trampoline(uint32_t hi, uint32_t lo) noexcept {
    // Reconstruct pointer from two ints (portable on 32 and 64-bit)
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    Fiber* f = reinterpret_cast<Fiber*>(ptr);
    try {
        f->fn()();
    } catch (const std::exception& ex) {
        Logger::instance().log(LogLevel::LOG_ERROR, "fiber",
            std::string("fiber panic: ") + ex.what());
    } catch (...) {
        Logger::instance().log(LogLevel::LOG_ERROR, "fiber",
            "fiber panic: unknown exception");
    }
    f->done_ref() = true;
    tl_current_fiber = nullptr;
    // CRITICAL: swap back to caller — uc_link=nullptr means we must do this
    // manually, otherwise the thread exits when the fiber function returns.
    ::swapcontext(&f->impl_->fiber_ctx, &f->impl_->caller_ctx);
    // This line is never reached — swapcontext does not return here.
}

// ── Fiber construction ────────────────────────────────────────────────────────

Fiber::Fiber(Fn fn, size_t stack_size)
    : impl_(std::make_unique<Impl>())
    , fn_(std::move(fn))
{
    const size_t page_size   = (size_t)::getpagesize();
    const size_t guard_pages = (GUARD_SIZE + page_size - 1) / page_size;
    const size_t guard_bytes = guard_pages * page_size;

    // Align stack size to page boundary
    const size_t aligned_stack = (stack_size + page_size - 1) / page_size * page_size;
    const size_t total_alloc   = guard_bytes + aligned_stack;

    // Allocate stack with mmap (necessary for mprotect)
    void* mem = ::mmap(nullptr, total_alloc,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
    if (mem == MAP_FAILED)
        throw std::runtime_error("fiber: mmap stack failed: " +
                                  std::string(std::strerror(errno)));

    impl_->stack_base  = mem;
    impl_->stack_alloc = total_alloc;
    impl_->stack_size  = aligned_stack;

    // Guard page at bottom (lowest address)
    install_guard_page();

    // Usable stack starts after guard page
    void* usable_stack = static_cast<char*>(mem) + guard_bytes;

    // Set up ucontext
    if (::getcontext(&impl_->fiber_ctx) != 0)
        throw std::runtime_error("fiber: getcontext failed");

    impl_->fiber_ctx.uc_stack.ss_sp   = usable_stack;
    impl_->fiber_ctx.uc_stack.ss_size = aligned_stack;
    impl_->fiber_ctx.uc_link          = nullptr;  // we manage return manually

    // Split this pointer into two uint32_t for makecontext
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    uint32_t  hi  = (uint32_t)(ptr >> 32);
    uint32_t  lo  = (uint32_t)(ptr & 0xFFFFFFFF);

    ::makecontext(&impl_->fiber_ctx,
                  reinterpret_cast<void(*)()>(fiber_trampoline),
                  2, hi, lo);
}

Fiber::~Fiber() {
    if (impl_ && impl_->stack_base) {
        remove_guard_page();
        ::munmap(impl_->stack_base, impl_->stack_alloc);
        impl_->stack_base = nullptr;
    }
}

std::shared_ptr<Fiber> Fiber::create(Fn fn, size_t stack_size) {
    // Can't use make_shared — constructor is private
    std::shared_ptr<Fiber> f(new Fiber(std::move(fn), stack_size));
    f->weak_self_ = f;  // weak reference to self for I/O resume callbacks
    return f;
}

// ── Context switch ────────────────────────────────────────────────────────────

void Fiber::resume() {
    assert(!done_ && "resuming a completed fiber");

    Fiber* prev        = tl_current_fiber;
    tl_current_fiber   = this;

    // Switch: save caller context into caller_ctx, jump to fiber_ctx
    ::swapcontext(&impl_->caller_ctx, &impl_->fiber_ctx);

    // Execution returns here when the fiber yields or completes
    tl_current_fiber = prev;
}

void Fiber::yield() {
    Fiber* self = tl_current_fiber;
    assert(self && "yield() called outside a fiber");

    // Switch back to the context that called resume()
    ::swapcontext(&self->impl_->fiber_ctx, &self->impl_->caller_ctx);
}

// ── FiberScheduler ────────────────────────────────────────────────────────────

FiberScheduler::FiberScheduler() {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    // Cross-thread wakeup: resume_fiber() yazar, epoll anında uyanır
    evfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epfd_ >= 0 && evfd_ >= 0) {
        epoll_event ev{};
        ev.events  = EPOLLIN;      // level-triggered — drain edilene kadar uyandırır
        ev.data.fd = evfd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, evfd_, &ev);
    }
}

FiberScheduler::~FiberScheduler() {
    if (evfd_ >= 0) { ::close(evfd_); evfd_ = -1; }
    if (epfd_ >= 0) { ::close(epfd_); epfd_ = -1; }
}

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

    // Yield dönüşü — fiber'ın akıbetine TEK yetkili burası karar verir.
    // resume_fiber() başka thread'den yalnızca WAITING durumundaki fiber'ı
    // taşıyabilir; SUSPENDING penceresinde sadece pending_resume_ bırakır.
    std::lock_guard<std::mutex> lk(mtx_);
    if (f->done()) {
        f->sched_state_.store(Fiber::SchedState::DONE, std::memory_order_release);
        // Olası artık kayıtları temizle (ör. suspend sonrası tamamlanma)
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
            // Resume, fiber yield etmeden geldi — waiting_'den çıkar, ready_'ye al
            auto wit = std::find(waiting_.begin(), waiting_.end(), f);
            if (wit != waiting_.end()) waiting_.erase(wit);
            auto pit = std::find(pool_waiters_.begin(), pool_waiters_.end(), f);
            if (pit != pool_waiters_.end()) pool_waiters_.erase(pit);
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        } else {
            // Suspend tamamlandı — artık resume_fiber taşıyabilir
            f->sched_state_.store(Fiber::SchedState::WAITING, std::memory_order_release);
        }
    } else {
        // Gönüllü yield (suspend çağrılmadı) — sıraya geri koy
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
    if (epfd_ < 0) return false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fd_to_fiber_[fd] = f;
        waiting_.push_back(f);
        f->sched_state_.store(Fiber::SchedState::SUSPENDING, std::memory_order_release);
    }
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = fd;
    // EPOLL_CTL_ADD: if fd already tracked (same connection, retry), use MOD
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST)
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    Fiber::yield();  // suspend — resumes from run_until_complete() after epoll fires
    return true;     // we were suspended and resumed — fd is now readable
}

void FiberScheduler::run_until_complete() {
    while (true) {
        while (run_one()) {}

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (waiting_.empty() && ready_.empty()) break;
        }

        // Ready varsa bekleme — hemen işle. Yoksa event bekle:
        // eventfd (cross-thread resume) veya DB soketi bizi anında uyandırır.
        // 20ms sadece güvenlik ağı (kaçan wakeup'a karşı üst sınır).
        int timeout_ms;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            timeout_ms = ready_.empty() ? 20 : 0;
        }

        epoll_event events[16];
        int n = epoll_wait(epfd_, events, 16, timeout_ms);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == evfd_) {
                // Cross-thread wakeup — sayacı boşalt, ready_ döngü başında işlenir
                uint64_t drain;
                while (::read(evfd_, &drain, sizeof(drain)) > 0) {}
                continue;
            }

            epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);

            std::shared_ptr<Fiber> f;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto it = fd_to_fiber_.find(fd);
                if (it != fd_to_fiber_.end()) {
                    f = it->second;
                    fd_to_fiber_.erase(it);
                }
            }
            if (f) resume_fiber(std::move(f));
        }
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
    // Erken gelen resume'u run_one() yield dönüşünde pending_resume_ ile işler —
    // burada ön-kontrol YOK: çift-enqueue penceresi tamamen kapalı.
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
        // Suspend tamamlanmış — tek güvenli taşıma yolu.
        auto it = std::find(waiting_.begin(), waiting_.end(), f);
        if (it != waiting_.end()) waiting_.erase(it);
        if (!f->done()) {
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        }
        cv_.notify_one();
        wake();
        return;
    }

    // SUSPENDING (yield dönüşü işlenmedi) veya RUNNING (suspend henüz kayıtsız):
    // ready_'ye DOKUNMA — bayrağı bırak, run_one() yield dönüşünde işlesin.
    // READY/DONE durumlarında bayrak zararsızdır (dönüşte temizlenir).
    f->pending_resume_.store(true, std::memory_order_release);
    cv_.notify_one();
    wake();
}

// epoll_wait'te uyuyan scheduler thread'ini anında uyandır (cross-thread çağrı)
void FiberScheduler::wake() {
    if (evfd_ >= 0) {
        uint64_t one = 1;
        ssize_t n = ::write(evfd_, &one, sizeof(one));
        (void)n;  // EAGAIN = sayaç dolu = zaten uyanacak
    }
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

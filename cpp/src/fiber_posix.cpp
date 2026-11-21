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
#include "look/mem_budget.h"   // request memory accounting at fiber switch
#include "look/logger.h"
#include "look/stack_guard.h"

#include <cassert>
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// POSIX
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>

// In an ASan build fiber switches are reported to ASan. If not reported, ASan does not recognize the fiber
// stack: when an exception is thrown (__asan_handle_no_return) it cleans the wrong stack, and the
// per-thread "fake stack" of the use-after-return check gets corrupted when fibers are suspended in the
// middle of a call and interleave with each other → false alarms. In a release
// build this block does not exist.
#if defined(__SANITIZE_ADDRESS__)
#  define LOOK_ASAN_FIBER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LOOK_ASAN_FIBER 1
#  endif
#endif
#ifdef LOOK_ASAN_FIBER
#  include <sanitizer/common_interface_defs.h>
#endif
// In a TSan build the switches are reported too: if not, TSan does not see the ucontext switch and takes one
// fiber's stack for another "thread's" → false race reports / crash. Each fiber is a
// TSan fiber; at a switch (flags=0) an ordering relation is established (they run sequentially on the same thread).
#if defined(__SANITIZE_THREAD__)
#  define LOOK_TSAN_FIBER 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define LOOK_TSAN_FIBER 1
#  endif
#endif
#ifdef LOOK_TSAN_FIBER
#  include <sanitizer/tsan_interface.h>
#endif

namespace look {

// ── Platform impl ─────────────────────────────────────────────────────────────

struct Fiber::Impl {
    ucontext_t fiber_ctx{};    // this fiber's execution context
    ucontext_t caller_ctx{};   // context to return to on yield()

    // Raw stack allocation (mmap for guard page support)
    void*  stack_base  = nullptr;  // bottom of allocation (guard page here)
    size_t stack_alloc = 0;        // total allocation size (stack + guard page)
    size_t stack_size  = 0;        // usable stack size (without guard page)
    void*  usable_stack = nullptr; // usable stack bottom (after the guard page)
#ifdef LOOK_ASAN_FIBER
    void*       fake_stack    = nullptr;   // the fiber's suspended fake stack
    const void* caller_bottom = nullptr;   // the (thread) stack bounds to return to
    size_t      caller_size   = 0;
#endif
#ifdef LOOK_TSAN_FIBER
    void* tsan_fiber  = nullptr;   // this fiber's TSan identity
    void* tsan_caller = nullptr;   // the TSan fiber of the (thread) that called resume()
#endif
};

// ── Thread-local state ────────────────────────────────────────────────────────

// fiber-ok: the scheduler itself — holds which fiber is running
static thread_local Fiber*           tl_current_fiber   = nullptr;
static thread_local FiberScheduler*  tl_scheduler       = nullptr;

Fiber* Fiber::current() noexcept { return tl_current_fiber; }

bool Fiber::stack_range(const char*& lo, const char*& hi) const noexcept {
    if (!impl_ || !impl_->usable_stack || !impl_->stack_size) return false;
    lo = static_cast<const char*>(impl_->usable_stack);
    hi = lo + impl_->stack_size;
    return true;
}

// look/stack_guard.h: the current stack — the fiber's or the thread's.
bool look_read_stack_range(const char*& lo, const char*& hi) noexcept {
    if (Fiber* f = Fiber::current()) { if (f->stack_range(lo, hi)) return true; }
#if defined(__APPLE__)
    pthread_t t = pthread_self();
    hi = static_cast<const char*>(pthread_get_stackaddr_np(t));
    lo = hi - pthread_get_stacksize_np(t);
    return true;
#else
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return false;
    void* addr = nullptr; size_t size = 0;
    const bool ok = pthread_attr_getstack(&a, &addr, &size) == 0 && addr && size;
    pthread_attr_destroy(&a);
    if (!ok) return false;
    lo = static_cast<const char*>(addr); hi = lo + size;
    return true;
#endif
}

FiberScheduler* get_thread_scheduler() noexcept { return tl_scheduler; }

// ── Yielding the turn (see fiber.h preempt_point) ─────────────────────────────
static constexpr int PREEMPT_CHECK_EVERY = 1024;
// fiber-ok: the time-slice counter/start belongs to the thread; re-established at a fiber switch
thread_local int t_preempt_budget = PREEMPT_CHECK_EVERY;
// The moment the fiber currently running on this thread last took over (run_one writes it before resume).
static thread_local std::chrono::steady_clock::time_point tl_slice_start;

static bool preempt_every_point() {
    static const bool v = [] {
        const char* e = std::getenv("LOOK_TEST_PREEMPT");
        return e && std::strcmp(e, "1") == 0;
    }();
    return v;
}

PreemptHook g_preempt_hook = nullptr;   // set by parallel_runtime.cpp (during static initialization)
void preempt_slow() noexcept {
    if (PreemptHook h = g_preempt_hook) h();   // end of script: the pure computation of a parallel() task is bounded
    const bool every = preempt_every_point();
    t_preempt_budget = every ? 1 : PREEMPT_CHECK_EVERY;
    if (!tl_current_fiber || !tl_scheduler) return;   // outside a fiber: no turn is yielded
    if (!every && std::chrono::steady_clock::now() - tl_slice_start < std::chrono::milliseconds(1))
        return;
    Fiber::yield();   // voluntary: the scheduler puts the fiber at the end of the ready queue
}

// ── Shared pool for long C++ jobs (see fiber.h fiber_offload) ─────────────────
// Fixed size (core count, at least 2): a flood of entry attempts does not open unbounded threads,
// the excess waits in the queue (fibers are suspended — threads stay open to other requests).
namespace {
struct OffloadPool {
    std::mutex                        m;
    std::condition_variable           cv;
    std::vector<std::function<void()>> q;
    OffloadPool() {
        unsigned n = std::thread::hardware_concurrency();
        if (n < 2) n = 2;
        for (unsigned i = 0; i < n; ++i)
            std::thread([this] {
                for (;;) {
                    std::function<void()> j;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cv.wait(lk, [&] { return !q.empty(); });
                        j = std::move(q.front());
                        q.erase(q.begin());
                    }
                    j();
                }
            }).detach();   // lives for the whole process lifetime
    }
    void post(std::function<void()> j) {
        { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(j)); }
        cv.notify_one();
    }
};
OffloadPool& offload_pool() { static OffloadPool* p = new OffloadPool(); return *p; }   // intentional leak: no destruction race at exit
}

void fiber_offload_run(const std::function<void()>& job) {
    Fiber* cur = tl_current_fiber;
    FiberScheduler* sched = tl_scheduler;
    std::shared_ptr<Fiber> sp = (cur && sched) ? cur->shared_from_this_fiber() : nullptr;
    if (!sp) { job(); return; }
    std::exception_ptr err;
    std::atomic<bool> done{false};
    // The request's memory accounting passes to the pool with the job (look/mem_budget.h): allocations in the pool
    // are charged to the request's budget. Single writer is preserved — the fiber is suspended until the job finishes.
    MemAccount* acct = mem_current_account();
    offload_pool().post([&, sched, sp, acct] {
        MemAccount* prev = mem_current_account();
        mem_set_current_account(acct);
        try { job(); } catch (...) { err = std::current_exception(); }
        mem_set_current_account(prev);
        done.store(true, std::memory_order_release);
        sched->resume_fiber(sp);
    });
    // Suspend at least once (an early wake-up is consumed in this suspend via pending_resume_);
    // on an unexpected wake-up before the job finishes, suspend again (see sqlite_client on_own_thread).
    do { FiberScheduler::suspend_current(); } while (!done.load(std::memory_order_acquire));
    if (err) std::rethrow_exception(err);
}
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
#ifdef LOOK_ASAN_FIBER
    __sanitizer_finish_switch_fiber(nullptr, &f->impl_->caller_bottom, &f->impl_->caller_size);
#endif
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
#ifdef LOOK_ASAN_FIBER
    __sanitizer_start_switch_fiber(nullptr, f->impl_->caller_bottom, f->impl_->caller_size);   // final exit
#endif
#ifdef LOOK_TSAN_FIBER
    __tsan_switch_to_fiber(f->impl_->tsan_caller, 0);
#endif
    ::swapcontext(&f->impl_->fiber_ctx, &f->impl_->caller_ctx);
    // This line is never reached — swapcontext does not return here.
}

// ── Stack cache ───────────────────────────────────────────────────────────────
// Every connection is a fiber; its stack was opened with mmap + mprotect and closed with munmap. Measurement
// (gdb sampling, WS connection storm): most of the fiber threads' time is in Fiber::Fiber
// — mmap, mprotect and the first touch of the new page (makecontext). A finished stack (guard
// page in place) returns to a PROCESS-WIDE cache (a fiber is most often destroyed on a thread other than
// the one that opened it). Measurement (160k WS storm, 16 cores): without cache 15k connections/s;
// cap 256 → 40k; 1024 → 53k; 4096 → 86k; 16384 → 95k.
// Memory: the cache allocates no new memory; it holds the stacks left behind by fibers that ran at the same time
// (as many as peak concurrency, at most 4096). At most KEEP of each stack stays touched ("warm").
// That memory is given back (madvise; the stack itself stays cached, guard page in place, and is reused cold):
//   * when idle: every PERIOD the stacks not taken during the whole period (the bottom of the LIFO, below the
//     period's low-water mark) are given back — after a storm, memory falls back within two periods even under
//     light traffic (measured: 1000 connections left 20 MB held for good; Go's scavenger does the same);
//   * under the memory budget (look/mem_budget.h): a warm stack is kept only while counted heap + the cache's
//     upper bound (warm × KEEP) stays under 90% of the limit, and when heap growth crosses it every warm stack is
//     given back at once — the cache never pushes the process past a container limit the budget stays under.
namespace {
struct StackCache {
    static constexpr size_t MAX = 4096;
    static constexpr auto PERIOD = std::chrono::seconds(5);
    struct Entry { void* p; size_t n; bool warm; };
    std::mutex m;
    std::condition_variable cv;
    std::vector<Entry> free_;      // LIFO: the back is the most recently returned
    size_t warm_ = 0;              // entries that may hold up to KEEP touched
    size_t low_ = 0;               // fewest entries during the current period: [0, low_) were never taken
    std::atomic<bool> pressure_{false};
    std::once_flag started_;
    StackCache() { free_.reserve(MAX); }   // give() never allocates under the lock
    void publish_held() { mem_held_set((int64_t)(warm_ * KEEP)); }
    void* take(size_t n) {
        std::lock_guard<std::mutex> lk(m);
        for (size_t i = free_.size(); i-- > 0;)
            if (free_[i].n == n) {
                void* p = free_[i].p;
                if (free_[i].warm) { --warm_; publish_held(); }
                free_[i] = free_.back(); free_.pop_back();
                if (free_.size() < low_) low_ = free_.size();
                return p;
            }
        return nullptr;
    }
    // Gives back the touched pages of the warm entries in [0, upto). Under the lock: an entry being
    // trimmed cannot be taken by a fiber that would then write to it.
    void trim_locked(size_t upto) {
        const size_t page = (size_t)::getpagesize();
        const size_t guard = (Fiber::GUARD_SIZE + page - 1) / page * page;
        for (size_t i = 0; i < upto && i < free_.size(); ++i) {
            Entry& e = free_[i];
            if (!e.warm) continue;
            ::madvise(static_cast<char*>(e.p) + guard, e.n - guard, MADV_DONTNEED);
            e.warm = false; --warm_;
        }
        publish_held();
    }
    // Called from the allocation path (mem_budget.h): no allocation, no lock, no throw.
    static void on_pressure() noexcept;
    void run() {
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
            if (warm_ == 0) cv.wait(lk, [&] { return warm_ > 0 || pressure_.load(); });
            low_ = free_.size();
            cv.wait_for(lk, PERIOD, [&] { return pressure_.load(); });
            if (pressure_.exchange(false)) trim_locked(free_.size());
            else trim_locked(low_);
        }
    }
    void start() {
        std::call_once(started_, [this] {
            mem_set_release_hook(&StackCache::on_pressure);
            std::thread([this] { run(); }).detach();
        });
    }
    // KEEP: from the top of the stack (high address; the stack grows downward) this much may stay touched
    // in the cache, the part below is returned with madvise: the megabytes of a fiber that did deep recursion
    // are not held in the cache (otherwise up to 4096 × 8 MB of memory could remain). madvise on every return
    // cut the accept rate to a third (84k → 29k/s), so only when needed: if the stack went
    // below KEEP, the BAND just below it has been written (the stack grows contiguously). Reading the band
    // is not a system call (a never-written page maps to the zero page); after madvise
    // the band is zero again. 96 KB: the connection handler's stack has a 64 KB read buffer
    // — in measurement an ordinary WS connection goes below 64 KB, but not below 72 KB.
    static constexpr size_t KEEP = 96 * 1024;
    static constexpr size_t BAND = 16 * 1024;
    // Reads the dead fiber's stack: ASan's stack redzones may have remained there.
#if defined(__SANITIZE_ADDRESS__)
    __attribute__((no_sanitize_address))
#endif
    static bool went_deep(const char* top) {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(top - KEEP - BAND);
        for (size_t i = 0; i < BAND / sizeof(uint64_t); ++i) if (w[i]) return true;
        return false;
    }
    bool give(void* p, size_t n) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (free_.size() >= MAX) return false;
        }
        start();
        const size_t page = (size_t)::getpagesize();
        const size_t guard = (Fiber::GUARD_SIZE + page - 1) / page * page;
        char* top = static_cast<char*>(p) + n;
        // Over the budget's room the stack is kept cold: its touched pages are given back now.
        const bool warm = mem_can_hold((int64_t)KEEP);
        if (!warm) ::madvise(static_cast<char*>(p) + guard, n - guard, MADV_DONTNEED);
        else if (n > guard + KEEP + BAND && went_deep(top))
            ::madvise(static_cast<char*>(p) + guard, n - guard - KEEP, MADV_DONTNEED);
        bool wake;
        {
            std::lock_guard<std::mutex> lk(m);
            if (free_.size() >= MAX) return false;
            free_.push_back({p, n, warm});
            wake = warm && warm_++ == 0;
            if (warm) publish_held();
        }
        if (wake) cv.notify_one();
        return true;
    }
};
// Lives for the whole process lifetime (leaked singleton): no destructor-order problem at exit.
StackCache& stack_cache() { static StackCache* c = new StackCache(); return *c; }
void StackCache::on_pressure() noexcept {
    StackCache& c = stack_cache();
    if (c.pressure_.exchange(true)) return;
    // Empty critical section: the trimmer either saw the flag or is already waiting, so the notify is not lost.
    // Taking the lock here cannot deadlock: nothing allocates while holding it (free_ is reserved up front).
    { std::lock_guard<std::mutex> lk(c.m); }
    c.cv.notify_one();
}
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

    // If there is a stack of the same size in the cache, take it (the guard page is already set up).
    void* mem = stack_cache().take(total_alloc);
    const bool reused = mem != nullptr;
    // Allocate stack with mmap (necessary for mprotect)
    if (!reused) mem = ::mmap(nullptr, total_alloc,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                       -1, 0);
    if (mem == MAP_FAILED)
        throw std::runtime_error("fiber: mmap stack failed: " +
                                  std::string(std::strerror(errno)));

    impl_->stack_base  = mem;
    impl_->stack_alloc = total_alloc;
    impl_->stack_size  = aligned_stack;
    mem_stacks_add((int64_t)StackCache::KEEP);   // a running stack counts in the memory budget (mem_budget.h)

    // Guard page at bottom (lowest address)
    if (!reused) install_guard_page();

    // Usable stack starts after guard page
    void* usable_stack = static_cast<char*>(mem) + guard_bytes;
    impl_->usable_stack = usable_stack;

    // Set up ucontext
    if (::getcontext(&impl_->fiber_ctx) != 0)
        throw std::runtime_error("fiber: getcontext failed");

    impl_->fiber_ctx.uc_stack.ss_sp   = usable_stack;
    impl_->fiber_ctx.uc_stack.ss_size = aligned_stack;
    impl_->fiber_ctx.uc_link          = nullptr;  // we manage return manually
#ifdef LOOK_TSAN_FIBER
    impl_->tsan_fiber = __tsan_create_fiber(0);
#endif

    // Split this pointer into two uint32_t for makecontext
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    uint32_t  hi  = (uint32_t)(ptr >> 32);
    uint32_t  lo  = (uint32_t)(ptr & 0xFFFFFFFF);

    ::makecontext(&impl_->fiber_ctx,
                  reinterpret_cast<void(*)()>(fiber_trampoline),
                  2, hi, lo);
}

Fiber::~Fiber() {
#ifdef LOOK_TSAN_FIBER
    if (impl_ && impl_->tsan_fiber) { __tsan_destroy_fiber(impl_->tsan_fiber); impl_->tsan_fiber = nullptr; }
#endif
    if (impl_ && impl_->stack_base) {
        mem_stacks_add(-(int64_t)StackCache::KEEP);
        if (stack_cache().give(impl_->stack_base, impl_->stack_alloc)) { impl_->stack_base = nullptr; return; }
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
    // Memory accounting passes along with the fiber (look/mem_budget.h)
    MemAccount* prev_acct = mem_current_account();
    mem_set_current_account(static_cast<MemAccount*>(local.mem_acct));

    // Switch: save caller context into caller_ctx, jump to fiber_ctx
#ifdef LOOK_ASAN_FIBER
    void* caller_fake = nullptr;
    __sanitizer_start_switch_fiber(&caller_fake, impl_->usable_stack, impl_->stack_size);
#endif
#ifdef LOOK_TSAN_FIBER
    impl_->tsan_caller = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(impl_->tsan_fiber, 0);
#endif
    ::swapcontext(&impl_->caller_ctx, &impl_->fiber_ctx);
#ifdef LOOK_ASAN_FIBER
    __sanitizer_finish_switch_fiber(caller_fake, nullptr, nullptr);
#endif

    // Execution returns here when the fiber yields or completes
    tl_current_fiber = prev;
    mem_set_current_account(prev_acct);
}

void Fiber::yield() {
    Fiber* self = tl_current_fiber;
    assert(self && "yield() called outside a fiber");

    // Switch back to the context that called resume()
#ifdef LOOK_ASAN_FIBER
    __sanitizer_start_switch_fiber(&self->impl_->fake_stack,
                                   self->impl_->caller_bottom, self->impl_->caller_size);
#endif
#ifdef LOOK_TSAN_FIBER
    __tsan_switch_to_fiber(self->impl_->tsan_caller, 0);
#endif
    ::swapcontext(&self->impl_->fiber_ctx, &self->impl_->caller_ctx);
#ifdef LOOK_ASAN_FIBER
    __sanitizer_finish_switch_fiber(self->impl_->fake_stack,
                                    &self->impl_->caller_bottom, &self->impl_->caller_size);
#endif
}

// ── FiberScheduler ────────────────────────────────────────────────────────────

FiberScheduler::FiberScheduler() {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    // Cross-thread wakeup: resume_fiber() writes, epoll wakes immediately
    evfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epfd_ >= 0 && evfd_ >= 0) {
        epoll_event ev{};
        ev.events  = EPOLLIN;      // level-triggered — wakes until drained
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
    tl_slice_start = std::chrono::steady_clock::now();   // the yield-the-turn measurement starts from this moment
    running_since_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        tl_slice_start.time_since_epoch()).count(), std::memory_order_relaxed);
    f->resume();
    running_since_ns_.store(0, std::memory_order_relaxed);

    // Yield return — ONLY this place decides the fiber's fate.
    // resume_fiber() from another thread can move only a fiber in the WAITING state;
    // in the SUSPENDING window it only leaves pending_resume_.
    std::lock_guard<std::mutex> lk(mtx_);
    if (f->done()) {
        f->sched_state_.store(Fiber::SchedState::DONE, std::memory_order_release);
        // Clean up possible leftover records (e.g. completion after suspend)
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
            // Resume came without the fiber yielding — remove it from waiting_, put it in ready_
            auto wit = std::find(waiting_.begin(), waiting_.end(), f);
            if (wit != waiting_.end()) waiting_.erase(wit);
            auto pit = std::find(pool_waiters_.begin(), pool_waiters_.end(), f);
            if (pit != pool_waiters_.end()) pool_waiters_.erase(pit);
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        } else {
            // Suspend completed — resume_fiber can now move it
            f->sched_state_.store(Fiber::SchedState::WAITING, std::memory_order_release);
        }
    } else {
        // Voluntary yield (suspend not called) — put it back in the queue
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
    return wait_readable_tmo(std::move(f), fd, -1) == 1;
}

// D Fix #2: time-limited wait. fd>=0 → epoll + (optional) deadline; fd<0 → pure
// timer (synthetic key, no epoll registration — the acceptor's backpressure sleep).
// Return: 1 readable, 0 timed out, -1 unsupported.
int FiberScheduler::wait_readable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms) {
    return wait_fd_tmo(std::move(f), fd, timeout_ms, EPOLLIN);
}

// Only suspend the fiber until it is writable (e.g. until a non-blocking connect finishes).
int FiberScheduler::wait_writable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms) {
    if (fd < 0) return -1;
    return wait_fd_tmo(std::move(f), fd, timeout_ms, EPOLLOUT);
}

int FiberScheduler::wait_fd_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms, uint32_t events) {
    if (epfd_ < 0) return -1;
    if (fd < 0 && timeout_ms < 0) return -1;   // fd-less indefinite wait is meaningless
    int key = fd;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (fd < 0) key = timer_key_seq_--;    // pure timer: a non-colliding key
        fd_to_fiber_[key] = f;
        if (timeout_ms >= 0)
            fd_deadline_[key] = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(timeout_ms);
        waiting_.push_back(f);
        f->sched_state_.store(Fiber::SchedState::SUSPENDING, std::memory_order_release);
    }
    if (fd >= 0) {
        epoll_event ev{};
        ev.events  = events | EPOLLONESHOT;
        ev.data.fd = fd;
        // EPOLL_CTL_ADD: if fd already tracked (same connection, retry), use MOD
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST)
            epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }
    Fiber::yield();  // suspend — an epoll event OR a deadline expiry resumes it
    Fiber* cur = Fiber::current();
    bool timed_out = cur && cur->io_timed_out_;
    if (cur) cur->io_timed_out_ = false;
    return timed_out ? 0 : 1;
}

void FiberScheduler::run_until_complete() {
    while (true) {
        // Run ready fibers — but at most 1 ms, then poll epoll. An unbounded "run until the queue
        // is empty" cannot be written: a fiber voluntarily yielding its turn (a long SQLite computation,
        // on_progress) re-enters the queue, the queue never empties, and a new connection / readable socket
        // would go unnoticed until the query finished (1.0.22: during a 1 s computation an ordinary request waited
        // ~0.8 s). Bounded by time (not by round count): under normal load the queue
        // empties before 1 ms is up and there is no extra epoll call (polling after every round lowered PG load by ~2%).
        const auto slice_start = std::chrono::steady_clock::now();
        while (run_one()) {
            if (std::chrono::steady_clock::now() - slice_start >= std::chrono::milliseconds(1)) break;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (waiting_.empty() && ready_.empty()) break;
        }

        // If there is anything ready, do not wait — process it right away. Otherwise wait for an event:
        // the eventfd (cross-thread resume) or a DB socket wakes us immediately.
        // 20ms safety net; if the nearest wait_readable_tmo deadline is closer
        // shorten to it (deadline precision ≤20ms — ample for idle timeout).
        int timeout_ms;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            timeout_ms = ready_.empty() ? 20 : 0;
            if (timeout_ms > 0 && !fd_deadline_.empty()) {
                auto now = std::chrono::steady_clock::now();
                for (auto& [k, dl] : fd_deadline_) {
                    auto ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(dl - now).count();
                    if (ms < timeout_ms) timeout_ms = ms < 0 ? 0 : ms;
                }
            }
        }

        epoll_event events[16];
        int n = epoll_wait(epfd_, events, 16, timeout_ms);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == evfd_) {
                // Cross-thread wakeup — drain the counter, ready_ is processed at the start of the loop
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
                fd_deadline_.erase(fd);   // event arrived → cancel the deadline
            }
            if (f) resume_fiber(std::move(f));
        }

        // Deadline sweep (D Fix #2): resume the expired ones with io_timed_out_.
        // If the event arrived above, their entries were already deleted → a double resume is impossible.
        {
            std::vector<std::pair<int, std::shared_ptr<Fiber>>> expired;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!fd_deadline_.empty()) {
                    auto now = std::chrono::steady_clock::now();
                    for (auto it = fd_deadline_.begin(); it != fd_deadline_.end(); ) {
                        if (it->second <= now) {
                            int key = it->first;
                            auto fit = fd_to_fiber_.find(key);
                            if (fit != fd_to_fiber_.end()) {
                                expired.emplace_back(key, fit->second);
                                fd_to_fiber_.erase(fit);
                            }
                            it = fd_deadline_.erase(it);
                        } else ++it;
                    }
                }
            }
            for (auto& [key, f] : expired) {
                if (key >= 0) epoll_ctl(epfd_, EPOLL_CTL_DEL, key, nullptr); // a pure timer has no registration
                f->io_timed_out_ = true;
                resume_fiber(std::move(f));
            }
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
    // An early resume is handled by run_one() at the yield return via pending_resume_ —
    // there is NO pre-check here: the double-enqueue window is completely closed.
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
        // Suspend completed — the only safe way to move it.
        auto it = std::find(waiting_.begin(), waiting_.end(), f);
        if (it != waiting_.end()) waiting_.erase(it);
        if (!f->done()) {
            f->sched_state_.store(Fiber::SchedState::READY, std::memory_order_release);
            ready_.push_back(std::move(f));
        }
        cv_.notify_one();
        wake_if_other_thread();
        return;
    }

    // SUSPENDING (yield return not yet processed) or RUNNING (suspend not yet recorded):
    // DO NOT TOUCH ready_ — leave the flag, let run_one() process it at the yield return.
    // In READY/DONE states the flag is harmless (cleared at the return).
    f->pending_resume_.store(true, std::memory_order_release);
    cv_.notify_one();
    wake_if_other_thread();
}

// Waking (eventfd write) is needed only from ANOTHER thread: this scheduler's own
// thread is running code here at that moment, it cannot be asleep in epoll_wait; the loop processes the ready queue
// without waiting (timeout 0 while ready_ is non-empty). It used to do a write + two reads to itself
// for every fiber resumed by a socket event — three unnecessary system calls per request.
void FiberScheduler::wake_if_other_thread() {
    if (get_thread_scheduler() != this) wake();
}

// wake the scheduler thread sleeping in epoll_wait immediately (cross-thread call)
void FiberScheduler::wake() {
    if (evfd_ >= 0) {
        uint64_t one = 1;
        ssize_t n = ::write(evfd_, &one, sizeof(one));
        (void)n;  // EAGAIN = counter full = it will wake anyway
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

#pragma once

// fiber.h — Stackful fiber abstraction
//
// Platform backends:
//   Linux / macOS / Docker: fiber_posix.cpp  (ucontext_t)
//   Windows native:         fiber_win.cpp    (CreateFiber/SwitchToFiber)
//
// Design decisions:
//   • Guard page protection — stack overflow → SIGSEGV (not silent corruption)
//   • FiberLocal storage — replaces thread_local for fiber-aware state
//   • Cooperative scheduling — yield() gives control back to scheduler
//   • No heap allocation inside yield/resume — hot path stays allocation-free
//
// Usage:
//   auto f = Fiber::create([]{
//       // ... do work ...
//       Fiber::yield();   // suspend, return control to caller
//       // ... resumed later ...
//   });
//   f->resume();   // run until next yield() or completion
//   f->resume();   // continue

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
// windows.h is required for WINAPI/VOID/LPVOID (the signature of the fiber_trampoline friend).
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace look {

// Forward declarations
class Fiber;
class DbConnection;

// ── FiberLocal — per-fiber storage (replaces thread_local) ───────────────────
//
// thread_local breaks when multiple fibers share a thread:
//   Fiber A sets tl_conns["db"] = conn_A, yields
//   Fiber B sets tl_conns["db"] = conn_B, yields
//   Fiber A resumes → tl_conns["db"] is now conn_B  ← data corruption
//
// FiberLocal is carried by each Fiber, accessed via Fiber::current()->local

struct FiberLocal {
    // DB connections — previously thread_local tl_conns
    std::map<std::string, std::shared_ptr<DbConnection>> conns;

    // Nesting depth of request dispatch + the outer request's context (http_main look_app_dispatch).
    // CANNOT be thread_local: the suspended request of another fiber on the same thread
    // would count as "nested" → it would not release its connection, the pool would run out, requests would hang.
    int   dispatch_depth = 0;
    void* dispatch_web   = nullptr;   // look::WebContext*
    void* dispatch_out   = nullptr;   // std::ostringstream*

    // State of the directly called callback (http_main DirectState: closure, arguments,
    // result, error, WS/SSE connection). CANNOT be thread_local: when the handler was suspended in a query
    // another fiber on the same thread started its own job and wrote over it →
    // the job was counted as "failed" and ran again (tests/jobs_concurrent_test.sh).
    std::shared_ptr<void> http_direct;

    // The VM currently running in this fiber (the fiber counterpart of vm.cpp's t_active_vm). CANNOT be thread_local:
    // when a request is suspended in a query, another request's VM on the same thread would overwrite it,
    // and a built-in with a callback (array::map …) would run the closure in that VM — in the other request's context.
    void* active_vm = nullptr;   // look::VM*

    // Request handle (look/request_scope.h): web modules (request::, response::, session:: …)
    // and print read the context and the output from here AT CALL TIME. So the modules are set up
    // once per thread (they used to be set up again on every request — most of the
    // request cost). CANNOT be thread_local: another suspended fiber's request would be read.
    void* req_web = nullptr;   // look::WebContext*
    void* req_out = nullptr;   // std::ostream*

    // The request's memory accounting (look/mem_budget.h). Fiber::resume switches the thread's current account
    // to this and restores it on return: allocations are not written to another suspended request.
    void* mem_acct = nullptr;  // look::MemAccount*

    // Extend here for other fiber-specific state as needed
    // (e.g., per-request log context, tracing IDs)

    void clear() { conns.clear(); }
};

// ── Fiber ─────────────────────────────────────────────────────────────────────

class Fiber {
public:
    using Fn = std::function<void()>;

    // Stack size constants
    // 8 MB, as large as a thread stack (1.0.20). It was 128 KB: ~100 levels of recursion in the interpreter
    // overflowed the fiber stack and crashed the WHOLE process (measured), before the depth protection
    // (MAX_CALL_DEPTH) fired. Only address space is reserved (MAP_NORESERVE): memory is consumed as
    // much as pages are used — the fiber of an idle waiting connection uses a few pages.
    static constexpr size_t STACK_DEFAULT = 8 * 1024 * 1024;   //  8 MB (address space)
    static constexpr size_t STACK_LARGE   = 512 * 1024;   //  512 KB
    static constexpr size_t STACK_MIN     =  64 * 1024;   //   64 KB
    static constexpr size_t GUARD_SIZE    =   4 * 1024;   //    4 KB (1 page)

    // Non-copyable, movable
    Fiber(const Fiber&)            = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&)                 = default;
    Fiber& operator=(Fiber&&)      = default;

    ~Fiber();

    // Factory — creates a fiber, does NOT start it yet
    // Call resume() to start execution
    static std::shared_ptr<Fiber> create(Fn fn,
                                          size_t stack_size = STACK_DEFAULT);

    // Retrieve the shared_ptr for this fiber (valid only if created via create())
    // Used by I/O completion handlers to safely resume a suspended fiber.
    std::shared_ptr<Fiber> shared_from_this_fiber() const {
        return weak_self_.lock();
    }

    // Internal accessors for platform backends
    Fn&   fn()      noexcept { return fn_; }
    bool& done_ref() noexcept { return done_; }

    // Platform backend trampolines need access to impl_ and private members
#ifndef _WIN32
    friend void fiber_trampoline(uint32_t hi, uint32_t lo) noexcept;
#else
    friend VOID WINAPI fiber_trampoline(LPVOID param) noexcept;
#endif
    friend class FiberScheduler;

    // Resume this fiber (from thread context or another fiber)
    // Returns when fiber calls yield() or completes
    void resume();

    // Yield from inside a running fiber — suspends until next resume()
    // Must be called from within a fiber (Fiber::current() != nullptr)
    static void yield();

    // True if the fiber function has returned
    bool done() const noexcept { return done_; }

    // Fiber-local storage — safe to access from inside the fiber
    FiberLocal local;

    // Returns the currently executing fiber on this thread
    // Returns nullptr if called from thread context (not inside any fiber)
    static Fiber* current() noexcept;

    // The usable stack range [lo, hi) of this fiber — look/stack_guard.h measures the remaining stack.
    // false if unknown (Windows: the operating system writes the fiber stack into the thread bounds).
    bool stack_range(const char*& lo, const char*& hi) const noexcept;

private:
    explicit Fiber(Fn fn, size_t stack_size);

    // Platform-specific context — defined in fiber_posix.cpp / fiber_win.cpp
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Fn     fn_;
    bool   done_ = false;

    // Weak reference to self — set by create(), used for I/O resume callbacks
    std::weak_ptr<Fiber> weak_self_;

    // ── Scheduler state machine ───────────────────────────────────────────────
    // The root fix for the double-enqueue race: on return run_one() decides where the fiber goes
    // by this state, not by scanning a vector.
    //   READY      — in the ready_ queue
    //   RUNNING    — resumed right now, running
    //   SUSPENDING — added to waiting_ but the yield return has not been processed yet
    //   WAITING    — the yield return was processed, waiting for I/O/pool resume
    //   DONE       — completed
    enum class SchedState : uint8_t { READY, RUNNING, SUSPENDING, WAITING, DONE };
    std::atomic<SchedState> sched_state_{SchedState::READY};

    // true if it was resumed because the wait_readable_tmo deadline expired — the fiber reads and resets it
    // on return from yield (it means the time ran out, the fd is NOT readable).
    bool io_timed_out_ = false;

    // If resume_fiber() hits the SUSPENDING window (the fiber is in waiting_ but the
    // yield return has not been processed in run_one yet) it does NOT MOVE it to ready_ — it
    // leaves this flag. run_one() sees the flag on return and takes the fiber to ready_ itself.
    // So the same fiber entering ready_ twice is structurally impossible.
    std::atomic<bool> pending_resume_{false};

    // Entry point called by the platform backend
    static void fiber_entry(Fiber* self) noexcept;

    // Guard page: map a read-only page at the bottom of the stack
    // → stack overflow causes SIGSEGV instead of silent memory corruption
    void install_guard_page();
    void remove_guard_page();
};

// ── FiberScheduler — simple round-robin scheduler for a thread ────────────────
//
// Used by the HTTP worker thread to multiplex multiple in-flight requests.
// Each worker thread has one FiberScheduler.
//
// Lifecycle:
//   scheduler.spawn(fn)   — create and enqueue a fiber
//   scheduler.run_until_idle() — resume fibers in FIFO order until all done
//   scheduler.run_one()   — resume next ready fiber, return false if none

class FiberScheduler {
public:
    FiberScheduler();
    ~FiberScheduler();

    FiberScheduler(const FiberScheduler&)            = delete;
    FiberScheduler& operator=(const FiberScheduler&) = delete;

    // Enqueue a new fiber to be run on this scheduler
    void spawn(Fiber::Fn fn, size_t stack_size = Fiber::STACK_DEFAULT);

    // Run the next ready fiber; returns true if any fiber was run
    bool run_one();

    // Run until no ready fibers remain (does NOT wait for I/O-suspended fibers)
    void run_until_idle();

    // Run until ALL fibers complete — including those suspended waiting for I/O.
    // Blocks the calling thread on a condition variable when fibers are waiting
    // for I/O and nothing is ready to run. I/O completion callbacks call
    // resume_fiber() which notifies the cv to wake this thread.
    void run_until_complete();

    // Called from inside a fiber when it has pending I/O (DB recv waiting)
    // Moves current fiber to wait_list; I/O completion will re-enqueue it
    static void suspend_current();

    // Block inside a fiber until fd is readable, then resume.
    // Uses the scheduler's own epoll — no dependency on the HTTP EventLoop.
    // Returns true if the fiber was suspended and resumed (fd is now readable).
    // Returns false if not supported on this platform (caller should use blocking recv).
    bool wait_readable(std::shared_ptr<Fiber> f, int fd);

    // wait_readable + TIME LIMIT (D Fix #2). Returns: 1 = fd readable, 0 = time
    // ran out (fd NOT readable), -1 = platform does not support it (blocking fallback).
    //   fd >= 0 → registers the fd in the scheduler epoll + sets a deadline.
    //   fd <  0 → PURE TIMER (no epoll registration): the fiber sleeps timeout_ms — for needs such as accept
    //             backpressure "back off for a short time" (no spin).
    // timeout_ms < 0 = unlimited (same as wait_readable). Idle keep-alive connection
    // fibers and the acceptor's shutdown check rely on this — waiting without a limit
    // was the root of the ab -k wind-down stall and of hanging at shutdown.
    int wait_readable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms);
    // The same, until writable (completion of a non-blocking connect). fd >= 0 required.
    int wait_writable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms);
    // The common body of the two (events: EPOLLIN / EPOLLOUT) — defined only on POSIX.
    int wait_fd_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms, uint32_t events);

    // Called from I/O completion (epoll callback) to wake a suspended fiber
    void resume_fiber(std::shared_ptr<Fiber> f);

    // Called when a DB pool slot becomes available.
    // Wakes the first fiber that was waiting for a pool connection.
    void notify_pool_waiter();

    // Wake the scheduler thread sleeping in epoll_wait immediately.
    // POSIX: writes to the eventfd; Windows: no-op (cv_ is already used).
    void wake();
    void wake_if_other_thread();   // wakes only when called from another thread

    // Called from inside a fiber when the DB pool is exhausted.
    // Moves fiber to pool_waiters_ and yields until notify_pool_waiter() is called.
    static void suspend_current_for_pool();

    // Current ready queue size
    size_t ready_count() const;

    // Total fibers (ready + suspended)
    size_t total_count() const;

    // The moment the currently running fiber started (steady_clock, ns); 0 = no fiber running. Read from another
    // thread (the server's watchdog): the VM loop gives up its turn every 1 ms, a long
    // built-in does not — if the value is stale the thread is stuck in an indivisible job.
    int64_t running_since_ns() const noexcept { return running_since_ns_.load(std::memory_order_relaxed); }

private:
    std::atomic<int64_t> running_since_ns_{0};
    // Ready queue — FIFO; fibers waiting to be resumed
    std::vector<std::shared_ptr<Fiber>> ready_;

    // Suspended fibers waiting for I/O
    std::vector<std::shared_ptr<Fiber>> waiting_;

    // Fibers waiting for a DB pool connection (pool was exhausted)
    std::vector<std::shared_ptr<Fiber>> pool_waiters_;

    // fd → waiting fiber — for fiber-owned epoll I/O wait
    // (key: a real fd >= 0, or a synthetic negative key for a pure timer)
    std::unordered_map<int, std::shared_ptr<Fiber>> fd_to_fiber_;

    // key → deadline (wait_readable_tmo). run_until_complete shortens the epoll timeout
    // to the nearest deadline; it resumes expired ones with io_timed_out_=true.
    std::unordered_map<int, std::chrono::steady_clock::time_point> fd_deadline_;
    int timer_key_seq_ = -2;   // synthetic keys for pure timers (-2, -3, ...)

    // Fiber-owned epoll fd for MySQL/DB socket readability — platform-specific
    // On POSIX: epoll fd; on Windows: unused (-1)
    int epfd_ = -1;

    // eventfd — cross-thread wakeup (POSIX). resume_fiber() writes,
    // run_until_complete wakes up in epoll immediately. The fix for the 1ms tick sleep
    // (6k plateau + inconsistent benchmark). Not used on Windows (-1).
    int evfd_ = -1;

    mutable std::mutex      mtx_;
    std::condition_variable cv_;   // notified by resume_fiber() when fiber becomes ready
};

// ── Thread-local scheduler access ────────────────────────────────────────────
//
// Each worker thread has its own scheduler.
// DB async code calls FiberScheduler::suspend_current() to yield to it.

FiberScheduler* get_thread_scheduler() noexcept;
void            set_thread_scheduler(FiberScheduler* s) noexcept;

// ── Giving up the turn for LOOK code (Elixir's "reduction" counter) ──────────────
// The VM's backward jumps and calls call preempt_point(). When the counter runs out (every 1024 points) the clock is checked: if the fiber has been running for more than 1 ms
// it gives up its turn — a long loop does not make other requests on the same thread
// wait (1.0.20–1.0.24: a 0.7 s loop made another request wait 0.35 s). Outside a fiber
// (CLI, old worker model, timer/parallel threads) only the counter decreases.
// LOOK_TEST_PREEMPT=1 is for TESTS only: the turn is given up at every point — it exposes thread-bound state
// getting mixed up between fibers (rare timing bugs) on every request.
// fiber-ok: the thread's preempt counter (fiber_posix.cpp), not request data
extern thread_local int t_preempt_budget;
void preempt_slow() noexcept;
// Hook called on the slow path (null = none). parallel_runtime.cpp sets it for the end-of-script check;
// the fiber layer does not know about tasks, it links on its own (tests compiled by hand in CI).
using PreemptHook = void (*)() noexcept;
extern PreemptHook g_preempt_hook;
inline void preempt_point() noexcept {
    if (--t_preempt_budget <= 0) preempt_slow();
}

// ── Long C++ work that cannot be split (Elixir's "dirty scheduler") ─────────────
// When a long built-in that cannot be interrupted in the middle (password hashing: PBKDF2 100k rounds ≈ 100 ms) is called inside a
// fiber it runs on a fixed-size shared thread pool, and the fiber is
// suspended meanwhile — the thread goes back to the other requests. Outside a fiber it runs directly. The job MUST NOT
// HOLD A LOCK and must not change SHARED LOOK values: it runs on another thread. It MAY produce NEW values
// (json::decode decodes a large document here; like parallel() tasks). The request's memory
// accounting travels with the job (look/mem_budget.h).
void fiber_offload_run(const std::function<void()>& job);
template <class F> auto fiber_offload(F&& f) -> decltype(f()) {
    decltype(f()) out{};
    fiber_offload_run([&] { out = f(); });
    return out;
}

} // namespace look

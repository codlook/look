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
#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
// WINAPI/VOID/LPVOID (fiber_trampoline friend imzası) için windows.h gerekli.
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

    // Extend here for other fiber-specific state as needed
    // (e.g., per-request log context, tracing IDs)

    void clear() { conns.clear(); }
};

// ── Fiber ─────────────────────────────────────────────────────────────────────

class Fiber {
public:
    using Fn = std::function<void()>;

    // Stack size constants
    static constexpr size_t STACK_DEFAULT = 128 * 1024;   //  128 KB
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

private:
    explicit Fiber(Fn fn, size_t stack_size);

    // Platform-specific context — defined in fiber_posix.cpp / fiber_win.cpp
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Fn     fn_;
    bool   done_ = false;

    // Weak reference to self — set by create(), used for I/O resume callbacks
    std::weak_ptr<Fiber> weak_self_;

    // ── Scheduler durum makinesi ─────────────────────────────────────────────
    // Çift-enqueue race'inin kökten çözümü: run_one() dönüşte fiber'ın nereye
    // gideceğine vector taramasıyla değil bu durumla karar verir.
    //   READY      — ready_ kuyruğunda
    //   RUNNING    — şu an resume edilmiş, çalışıyor
    //   SUSPENDING — waiting_'e eklendi ama yield dönüşü henüz işlenmedi
    //   WAITING    — yield dönüşü işlendi, I/O/pool resume bekliyor
    //   DONE       — tamamlandı
    enum class SchedState : uint8_t { READY, RUNNING, SUSPENDING, WAITING, DONE };
    std::atomic<SchedState> sched_state_{SchedState::READY};

    // wait_readable_tmo deadline'ı dolarak resume edildiyse true — fiber yield
    // dönüşünde okuyup sıfırlar (fd okunabilir DEĞİL, süre doldu demektir).
    bool io_timed_out_ = false;

    // resume_fiber() SUSPENDING penceresine denk gelirse (fiber waiting_'de ama
    // yield dönüşü run_one'da henüz işlenmedi) ready_'ye TAŞIMAZ — bu bayrağı
    // bırakır. run_one() dönüşte bayrağı görüp fiber'ı ready_'ye kendisi alır.
    // Böylece aynı fiber'ın ready_'ye iki kez girmesi yapısal olarak imkansız.
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

    // wait_readable + SÜRE SINIRI (D Fix #2). Dönüş: 1 = fd okunabilir, 0 = süre
    // doldu (fd okunabilir DEĞİL), -1 = platform desteklemiyor (blocking fallback).
    //   fd >= 0 → fd'yi scheduler epoll'una kaydeder + deadline kurar.
    //   fd <  0 → SAF ZAMANLAYICI (epoll kaydı yok): fiber timeout_ms uyur — accept
    //             backpressure gibi "kısa süre çekil" ihtiyaçları için (spin yok).
    // timeout_ms < 0 = süresiz (wait_readable ile aynı). İdle keep-alive bağlantı
    // fiber'ları ve acceptor'ın kapanış kontrolü buna dayanır — süresiz bekleme,
    // ab -k wind-down stall'ının ve kapanışta takılmanın köküydü.
    int wait_readable_tmo(std::shared_ptr<Fiber> f, int fd, int timeout_ms);

    // Called from I/O completion (epoll callback) to wake a suspended fiber
    void resume_fiber(std::shared_ptr<Fiber> f);

    // Called when a DB pool slot becomes available.
    // Wakes the first fiber that was waiting for a pool connection.
    void notify_pool_waiter();

    // epoll_wait'te uyuyan scheduler thread'ini anında uyandır.
    // POSIX: eventfd'ye yazar; Windows: no-op (cv_ zaten kullanılıyor).
    void wake();

    // Called from inside a fiber when the DB pool is exhausted.
    // Moves fiber to pool_waiters_ and yields until notify_pool_waiter() is called.
    static void suspend_current_for_pool();

    // Current ready queue size
    size_t ready_count() const;

    // Total fibers (ready + suspended)
    size_t total_count() const;

private:
    // Ready queue — FIFO; fibers waiting to be resumed
    std::vector<std::shared_ptr<Fiber>> ready_;

    // Suspended fibers waiting for I/O
    std::vector<std::shared_ptr<Fiber>> waiting_;

    // Fibers waiting for a DB pool connection (pool was exhausted)
    std::vector<std::shared_ptr<Fiber>> pool_waiters_;

    // fd → waiting fiber — for fiber-owned epoll I/O wait
    // (anahtar: gerçek fd >= 0, ya da saf-zamanlayıcı için sentetik negatif anahtar)
    std::unordered_map<int, std::shared_ptr<Fiber>> fd_to_fiber_;

    // anahtar → deadline (wait_readable_tmo). run_until_complete epoll timeout'unu
    // en yakın deadline'a göre kısar; dolanları io_timed_out_=true ile resume eder.
    std::unordered_map<int, std::chrono::steady_clock::time_point> fd_deadline_;
    int timer_key_seq_ = -2;   // saf-zamanlayıcı sentetik anahtarları (-2, -3, ...)

    // Fiber-owned epoll fd for MySQL/DB socket readability — platform-specific
    // On POSIX: epoll fd; on Windows: unused (-1)
    int epfd_ = -1;

    // eventfd — cross-thread wakeup (POSIX). resume_fiber() yazar,
    // run_until_complete() epoll'da anında uyanır. 1ms tick uykusunun
    // (6k plato + tutarsız benchmark) çözümü. Windows'ta kullanılmaz (-1).
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

} // namespace look

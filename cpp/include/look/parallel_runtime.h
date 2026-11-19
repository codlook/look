#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>

namespace look {

// ── Parallel task lifecycle ───────────────────────────────────────────────────
// Default limit: 64.  Override with env var LOOK_PARALLEL_LIMIT (integer).
// Small server: 32.  CPU-bound batch workers: 128.
// Note: SMTP / HTTP listeners are I/O-bound and must use event_loop, not parallel().

// Returns the active task limit (reads LOOK_PARALLEL_LIMIT once, then caches).
int task_limit();

// Acquire modes for task_acquire():
//   THROW — throws std::runtime_error when limit is reached (default, fast fail)
//   WAIT  — blocks until a slot is free (real backpressure, honours timeout_ms)
//   TRY   — returns false immediately if no slot available (caller decides)
enum class AcquireMode { THROW, WAIT, TRY };

// Reserve a task slot.
//   THROW: throws on limit. WAIT: blocks up to timeout_ms. TRY: returns false on limit.
// Returns true if acquired, false only in TRY mode when limit is full.
bool task_acquire(AcquireMode mode = AcquireMode::THROW, int timeout_ms = 0);

// Release a task slot and signal waiters.  Always pair with a successful acquire.
// Prefer TaskGuard — it calls this automatically.
void task_release() noexcept;

// Active task count (instantaneous snapshot).
int  task_active();

// Block until all tasks finish or timeout_ms elapses.
// Returns true if count reached 0, false on timeout.
bool task_wait(int timeout_ms = 5000);

// RAII guard: calls task_release() on scope exit regardless of exceptions.
struct TaskGuard {
    ~TaskGuard() { task_release(); }
};

// ── Script end: does a task still have work left? (main.cpp finish_script) ───
// When the script ends, a running task advances until the FIRST moment it either RETURNS or starts doing work that
// leaves a trace outside; time is ignored and the script does not wait for the task. "Leaves a trace" = the builtin's
// effect class is Effect (output, channel, file, network, database) or unclassified; Pure/Reads/
// ThrowCheck/Callback keep running (count(), string::upper...). Pure computation is bounded too:
// after the script ends, 16 pure builtins or 16 preempt slow paths (about 16k backward jumps/
// calls) -> it is stopped (an infinite loop is not waited for). Previously a task whose only step left after send() was returning
// was also counted as "still running": a single send, a loop, an if branch -- a 50/50 false warning.
// Channel: send moves the task to RUNNING right before it DELIVERS the value (with an unbuffered channel the receiver takes it).
enum : int { TASK_RUNNING = 0, TASK_IN_EFFECT = 1, TASK_DONE = 2, TASK_STOPPED = 3 };
struct TaskExit {
    std::atomic<int> phase{TASK_RUNNING};
    int after_budget = 16;   // written only by the task's own thread
};
TaskExit* task_exit_new();                 // registered in PARALLEL_CALL (before the thread starts)
void task_exit_enter(TaskExit* t) noexcept; // at the start of the task's thread (t_task_exit)
void task_exit_done(TaskExit* t) noexcept;  // task finished -> leave the registry, delete, t_task_exit = null
extern std::atomic<bool> g_task_script_ending;
inline bool task_script_ending() noexcept { return g_task_script_ending.load(); }
[[noreturn]] void task_stop_here(TaskExit* t) noexcept;
bool task_point_slow(TaskExit* t, bool effect) noexcept;
// Builtin/channel point. If it is an effect, moves to IN_EFFECT and returns true (then task_effect_end).
// Fast path inline: while the script runs, a pure builtin = a single read (+1.1% measured on a task -> ~0).
inline bool task_point(TaskExit* t, bool effect) noexcept {
    if (!effect && !g_task_script_ending.load(std::memory_order_relaxed)) return false;
    return task_point_slow(t, effect);
}
inline void task_effect_end(TaskExit* t) noexcept { t->phase.store(TASK_RUNNING); }
void task_preempt_check() noexcept;        // from preempt_slow (once every 1024 points)
// Script end: clear the finishing flag and wait until no RUNNING task remains. Returns the number of tasks with work left.
int task_settle_for_exit();
// fiber-ok: the thread's own task record (parallel() thread), not request data
extern thread_local TaskExit* t_task_exit;

// ── Channel deadlock (Go: "all goroutines are asleep - deadlock!") ───────────
// On the command line, if the script and all parallel() tasks are waiting on channels and there is no installed or
// running timer either, none can ever wake up again: the waiters are woken and all get an error (the main
// script's error is written with the call chain). Previously the process hung forever. Off on the web (with
// requests and fibers, "everyone is waiting" is undefined; there LOOK_CHANNEL_TIMEOUT_MS exists).
// Lock order: the channel's mtx -> the accounting lock; wake-ups happen AFTER the accounting lock is RELEASED.
struct LookChannel;
void chan_deadlock_main(bool on) noexcept;   // command line: true at script start, false at the end
extern std::atomic<bool> g_chan_deadlock;    // permanent once detected
using TimersActiveFn = bool (*)();
extern TimersActiveFn g_timers_active;       // set by timer_stdlib: is there an installed/running timer
const char* chan_deadlock_message() noexcept;
// For the duration of a channel wait (set up and torn down while the channel's lock is held). The counts (waiting ==
// alive) are only a SUSPICION: a waiter that has been signalled but has not yet woken is also counted (an
// unbuffered send signals the receiver and immediately starts waiting for delivery). The decision is made in
// chan_deadlock_try: each waiter's channel is read under its own lock -- if one can make progress there is
// no deadlock; if something changed in between, no decision is made (the one who changed it repeats the check).
enum ChanWaitKind : int { CHAN_WAIT_RECV = 0, CHAN_WAIT_ROOM = 1, CHAN_WAIT_TAKEN = 2 };
struct ChanWait {
    ChanWait(std::shared_ptr<LookChannel> ch, int kind, uint64_t gen);
    ~ChanWait();
    ChanWait(const ChanWait&) = delete;
    ChanWait& operator=(const ChanWait&) = delete;
    bool suspect = false;   // waiting == alive at entry -> channel lock RELEASED and chan_deadlock_try()
    std::shared_ptr<LookChannel> ch;
};
void chan_deadlock_try() noexcept;   // called with no channel lock held

} // namespace look

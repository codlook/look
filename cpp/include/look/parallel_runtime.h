#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace look {

// ── Parallel task lifecycle ───────────────────────────────────────────────────
// Shared between interpreter.cpp (tree-walk) and vm.cpp (bytecode).
//
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

} // namespace look

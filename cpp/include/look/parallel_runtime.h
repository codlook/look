#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace look {

// ── Goroutine lifecycle ───────────────────────────────────────────────────────
// Shared between interpreter.cpp (tree-walk) and vm.cpp (bytecode).
//
// Default limit: 64.  Override with env var LOOK_GOROUTINE_LIMIT (integer).
// Small server: 32.  CPU-bound batch workers: 128.
// Note: SMTP / HTTP listeners are I/O-bound and must use event_loop, not parallel().

// Returns the active goroutine limit (reads LOOK_GOROUTINE_LIMIT once, then caches).
int goroutine_limit();

// Acquire modes for goroutine_acquire():
//   THROW — throws std::runtime_error when limit is reached (default, fast fail)
//   WAIT  — blocks until a slot is free (real backpressure, honours timeout_ms)
//   TRY   — returns false immediately if no slot available (caller decides)
enum class AcquireMode { THROW, WAIT, TRY };

// Reserve a goroutine slot.
//   THROW: throws on limit. WAIT: blocks up to timeout_ms. TRY: returns false on limit.
// Returns true if acquired, false only in TRY mode when limit is full.
bool goroutine_acquire(AcquireMode mode = AcquireMode::THROW, int timeout_ms = 0);

// Release a goroutine slot and signal waiters.  Always pair with a successful acquire.
// Prefer GoroutineGuard — it calls this automatically.
void goroutine_release();

// Active goroutine count (instantaneous snapshot).
int  goroutine_active();

// Block until all goroutines finish or timeout_ms elapses.
// Returns true if count reached 0, false on timeout.
bool goroutine_wait(int timeout_ms = 5000);

// RAII guard: calls goroutine_release() on scope exit regardless of exceptions.
struct GoroutineGuard {
    ~GoroutineGuard() { goroutine_release(); }
};

} // namespace look

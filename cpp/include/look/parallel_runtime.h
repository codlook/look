#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace look {

// ── Goroutine lifecycle ───────────────────────────────────────────────────────
// shared between interpreter.cpp (tree-walk) and vm.cpp (bytecode).

static constexpr int PARALLEL_MAX_GOROUTINES = 64;

// Increment counter; throws std::runtime_error if limit is reached.
void goroutine_acquire();

// Decrement counter and signal waiters.  Always call after goroutine_acquire(),
// even on panic — use the RAII guard below instead of calling this directly.
void goroutine_release();

// Active goroutine count (instantaneous snapshot).
int  goroutine_active();

// Block until all goroutines finish or timeout_ms elapses.
// Returns true if count reached 0, false on timeout.
bool goroutine_wait(int timeout_ms = 5000);

// RAII guard: decrements on scope exit regardless of exceptions.
struct GoroutineGuard {
    ~GoroutineGuard() { goroutine_release(); }
};

} // namespace look

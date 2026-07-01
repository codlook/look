#include "look/parallel_runtime.h"
#include <stdexcept>
#include <string>
#include <cstdlib>

namespace look {

static std::atomic<int>        s_count{0};
static std::mutex              s_mtx;
static std::condition_variable s_cv;

int goroutine_limit() {
    // LOOK_GOROUTINE_LIMIT env var — parsed once, cached.
    // Consistent with LOOK_WS_MAX_CONN and LOOK_SSE_MAX_CONN patterns.
    static int limit = []() -> int {
        const char* e = std::getenv("LOOK_GOROUTINE_LIMIT");
        if (e && *e) {
            int v = std::atoi(e);
            if (v > 0 && v <= 4096) return v;
        }
        return 64; // default
    }();
    return limit;
}

bool goroutine_acquire(AcquireMode mode, int timeout_ms) {
    if (mode == AcquireMode::TRY) {
        // Optimistic increment — roll back if over limit
        int prev = s_count.fetch_add(1);
        if (prev >= goroutine_limit()) {
            s_count.fetch_sub(1);
            return false;
        }
        return true;
    }

    if (mode == AcquireMode::WAIT) {
        // Block until a slot opens or timeout elapses
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
        while (true) {
            int prev = s_count.fetch_add(1);
            if (prev < goroutine_limit()) return true;
            s_count.fetch_sub(1);
            std::unique_lock<std::mutex> lk(s_mtx);
            bool ok = s_cv.wait_until(lk, deadline,
                []{ return s_count.load() < goroutine_limit(); });
            if (!ok) {
                // Timeout — one last try
                prev = s_count.fetch_add(1);
                if (prev < goroutine_limit()) return true;
                s_count.fetch_sub(1);
                throw std::runtime_error(
                    "parallel(): backpressure timeout — goroutine slot bekleniyor (" +
                    std::to_string(timeout_ms) + " ms)");
            }
        }
    }

    // AcquireMode::THROW (default)
    int prev = s_count.fetch_add(1);
    if (prev >= goroutine_limit()) {
        s_count.fetch_sub(1);
        throw std::runtime_error(
            "parallel(): goroutine limiti aşıldı (" +
            std::to_string(goroutine_limit()) + "). " +
            "LOOK_GOROUTINE_LIMIT env ile artırılabilir.");
    }
    return true;
}

void goroutine_release() {
    int remaining = s_count.fetch_sub(1) - 1;
    if (remaining < goroutine_limit()) {
        // Wake up any WAIT-mode acquirers
        s_cv.notify_one();
    }
    if (remaining <= 0) {
        // Wake up goroutine_wait() callers
        std::lock_guard<std::mutex> lk(s_mtx);
        s_cv.notify_all();
    }
}

int goroutine_active() {
    return s_count.load();
}

bool goroutine_wait(int timeout_ms) {
    std::unique_lock<std::mutex> lk(s_mtx);
    return s_cv.wait_for(lk,
        std::chrono::milliseconds(timeout_ms),
        []{ return s_count.load() <= 0; });
}

} // namespace look

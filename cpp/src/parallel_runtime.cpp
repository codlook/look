#include "look/parallel_runtime.h"
#include <stdexcept>
#include <string>
#include <cstdlib>

namespace look {

static std::atomic<int>        s_count{0};
static std::mutex              s_mtx;
static std::condition_variable s_cv;

int task_limit() {
    // LOOK_PARALLEL_LIMIT env var — parsed once, cached.
    static int limit = []() -> int {
        const char* e = std::getenv("LOOK_PARALLEL_LIMIT");
        if (e && *e) {
            int v = std::atoi(e);
            if (v > 0 && v <= 4096) return v;
        }
        return 64; // default
    }();
    return limit;
}

bool task_acquire(AcquireMode mode, int timeout_ms) {
    if (mode == AcquireMode::TRY) {
        int prev = s_count.fetch_add(1);
        if (prev >= task_limit()) {
            s_count.fetch_sub(1);
            return false;
        }
        return true;
    }

    if (mode == AcquireMode::WAIT) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
        while (true) {
            int prev = s_count.fetch_add(1);
            if (prev < task_limit()) return true;
            s_count.fetch_sub(1);
            std::unique_lock<std::mutex> lk(s_mtx);
            bool ok = s_cv.wait_until(lk, deadline,
                []{ return s_count.load() < task_limit(); });
            if (!ok) {
                prev = s_count.fetch_add(1);
                if (prev < task_limit()) return true;
                s_count.fetch_sub(1);
                throw std::runtime_error(
                    "parallel(): backpressure timeout — task slot bekleniyor (" +
                    std::to_string(timeout_ms) + " ms)");
            }
        }
    }

    // AcquireMode::THROW (default)
    int prev = s_count.fetch_add(1);
    if (prev >= task_limit()) {
        s_count.fetch_sub(1);
        throw std::runtime_error(
            "parallel(): task limiti aşıldı (" +
            std::to_string(task_limit()) + "). " +
            "LOOK_PARALLEL_LIMIT env ile artırılabilir.");
    }
    return true;
}

void task_release() noexcept {
    int remaining = s_count.fetch_sub(1) - 1;
    if (remaining < task_limit()) {
        s_cv.notify_one();
    }
    if (remaining <= 0) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_cv.notify_all();
    }
}

int task_active() {
    return s_count.load();
}

bool task_wait(int timeout_ms) {
    std::unique_lock<std::mutex> lk(s_mtx);
    return s_cv.wait_for(lk,
        std::chrono::milliseconds(timeout_ms),
        []{ return s_count.load() <= 0; });
}

} // namespace look

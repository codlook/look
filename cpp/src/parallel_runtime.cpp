#include "look/parallel_runtime.h"
#include <stdexcept>
#include <string>

namespace look {

static std::atomic<int>      s_count{0};
static std::mutex            s_mtx;
static std::condition_variable s_cv;

void goroutine_acquire() {
    int prev = s_count.fetch_add(1);
    if (prev >= PARALLEL_MAX_GOROUTINES) {
        s_count.fetch_sub(1);
        throw std::runtime_error(
            "parallel(): goroutine limiti aşıldı (" +
            std::to_string(PARALLEL_MAX_GOROUTINES) + ")");
    }
}

void goroutine_release() {
    int remaining = s_count.fetch_sub(1) - 1;
    if (remaining <= 0) {
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

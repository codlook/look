#include "look/db_async_pool.h"
#include "look/logger.h"
#include <string>

namespace look {

AsyncDbPool::AsyncDbPool(int thread_count) {
    threads_.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i)
        threads_.emplace_back([this] { worker_loop(); });
    Logger::instance().log(LogLevel::LOG_INFO, "DB",
        "AsyncDbPool[" + std::to_string(thread_count) + "] başlatıldı");
}

AsyncDbPool::~AsyncDbPool() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

AsyncDbPool::Future AsyncDbPool::submit(QueryFn fn) {
    Task task;
    task.fn = std::move(fn);
    Future fut = task.promise.get_future();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push(std::move(task));
    }
    cv_.notify_one();
    return fut;
}

void AsyncDbPool::worker_loop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
        }
        active_.fetch_add(1, std::memory_order_relaxed);
        try {
            task.promise.set_value(task.fn());
        } catch (...) {
            task.promise.set_exception(std::current_exception());
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ── Global pool ───────────────────────────────────────────────────────────────

static std::unique_ptr<AsyncDbPool> g_async_db_pool;
static std::mutex                   g_async_pool_mtx;

void set_db_async_pool(std::unique_ptr<AsyncDbPool> pool) {
    std::lock_guard<std::mutex> lk(g_async_pool_mtx);
    g_async_db_pool = std::move(pool);
}

AsyncDbPool* get_db_async_pool() {
    return g_async_db_pool.get();  // lock gerekmez — startup'ta set, sonra read-only
}

} // namespace look

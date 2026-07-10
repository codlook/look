#pragma once
#include <future>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include "look/db_connection.h"

namespace look {

// AsyncDbPool — HTTP worker thread'lerinden bağımsız DB thread pool.
//
// HTTP worker sorguyu submit() ile gönderir, future.get() ile sonucu bekler.
// DB thread pool MySQL bağlantılarını tutar; workers << db_threads mümkün:
//   LOOK_WORKERS=16 LOOK_DB_ASYNC_THREADS=64 → 16 HTTP, 64 MySQL bağlantısı.
//
// NOT: future.get() HTTP thread'i bloklar. Gerçek kazanım = daha az HTTP
// thread context-switch (16 vs 64). Tam non-blocking için coroutine gerekir.
class AsyncDbPool {
public:
    using RowResult = std::vector<DbRow>;
    using QueryFn   = std::function<RowResult()>;
    using Future    = std::future<RowResult>;

    explicit AsyncDbPool(int thread_count);
    ~AsyncDbPool();

    // Sorguyu DB thread pool'una gönder — hemen future döner.
    // HTTP worker future.get() ile sonucu bekler (thread yield via OS scheduler).
    Future submit(QueryFn fn);

    int thread_count() const { return (int)threads_.size(); }
    int active()       const { return active_.load(std::memory_order_relaxed); }

private:
    struct Task {
        QueryFn                    fn;
        std::promise<RowResult>    promise;
    };

    void worker_loop();

    std::vector<std::thread>  threads_;
    std::queue<Task>          queue_;
    std::mutex                mtx_;
    std::condition_variable   cv_;
    std::atomic<bool>         stop_{false};
    std::atomic<int>          active_{0};
};

// Global async pool — set_db_async_pool() ile kurulur, get_db_async_pool() ile kullanılır.
void set_db_async_pool(std::unique_ptr<AsyncDbPool> pool);
AsyncDbPool* get_db_async_pool();

} // namespace look

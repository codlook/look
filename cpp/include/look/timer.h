#pragma once
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <map>
#include <chrono>

namespace look {

class TimerManager {
public:
    static TimerManager& instance();

    // Schedule one-shot callback after ms milliseconds. Returns timer id.
    int after(int ms, std::function<void()> cb);

    // Schedule repeating callback every ms milliseconds. Returns timer id.
    int every(int ms, std::function<void()> cb);

    // Cancel a scheduled timer.
    void cancel(int id);

    // Shutdown timer thread.
    void shutdown();

private:
    struct Entry {
        int id;
        std::chrono::steady_clock::time_point next_fire;
        int interval_ms = 0;   // 0 = one-shot, >0 = repeating
        std::function<void()> callback;
        bool cancelled = false;
    };

    TimerManager();
    ~TimerManager();
    void run();

    std::atomic<int>     next_id_{1};
    std::mutex           mtx_;
    std::condition_variable cv_;
    std::map<int, Entry> entries_;
    std::thread          thread_;
    std::atomic<bool>    running_{true};
};

} // namespace look

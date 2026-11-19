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
    // Has the manager (and thread) been set up in this process? The warning at script end must not start a
    // thread just to ask, in a script that uses no timers.
    static bool exists();

    // Schedule one-shot callback after ms milliseconds. Returns timer id.
    int after(int ms, std::function<void()> cb);

    // Schedule repeating callback every ms milliseconds. Returns timer id.
    int every(int ms, std::function<void()> cb);

    // Cancel a scheduled timer.
    void cancel(int id);

    // Shutdown timer thread.
    void shutdown();

    // (command line): timers still installed when the script ends -- one-shots (not yet
    // fired) and repeating ones. The warning at exit counts these.
    struct Pending { int once = 0; int repeating = 0; };
    Pending pending();
    // Is there an installed or currently running timer (channel deadlock: a callback may send).
    bool active();

    // timer::wait([ms]) -- waits until all one-shot timers have fired and finished their callbacks
    // (or until ms elapses); true = all finished. Does not wait for repeating ones (they never finish).
    bool wait_one_shots(int timeout_ms);

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
    bool                 dirty_ = false;   // under mtx_: the list changed -> the wait time is recomputed
    int                  running_cbs_ = 0; // under mtx_: number of one-shot callbacks currently running
    std::condition_variable idle_cv_;      // one-shot work decreased (fired/finished/cancelled) -> wait_one_shots
};

} // namespace look

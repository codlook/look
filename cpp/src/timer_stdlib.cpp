#include "look/timer.h"
#include "look/parallel_runtime.h"   // g_timers_active

namespace look {

static std::atomic<bool> g_timer_created{false};

TimerManager& TimerManager::instance() {
    static TimerManager inst;
    return inst;
}

bool TimerManager::exists() { return g_timer_created.load(); }

TimerManager::TimerManager() {
    g_timer_created.store(true);
    thread_ = std::thread([this] { run(); });
}

TimerManager::~TimerManager() {
    shutdown();
}

int TimerManager::after(int ms, std::function<void()> cb) {
    int id = next_id_.fetch_add(1);
    Entry e;
    e.id          = id;
    e.next_fire   = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    e.interval_ms = 0;
    e.callback    = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_[id] = std::move(e);
        dirty_ = true;
    }
    cv_.notify_one();
    return id;
}

int TimerManager::every(int ms, std::function<void()> cb) {
    int id = next_id_.fetch_add(1);
    Entry e;
    e.id          = id;
    e.next_fire   = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    e.interval_ms = ms;
    e.callback    = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_[id] = std::move(e);
        dirty_ = true;
    }
    cv_.notify_one();
    return id;
}

void TimerManager::cancel(int id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_.erase(id);   // Erase immediately -- the callback lambda ($ws shared_ptr) is released
        dirty_ = true;
    }
    cv_.notify_one();         // Wake the thread -- so the deadline is updated
    idle_cv_.notify_all();    // timer::wait must not wait for a cancelled one-shot
}

TimerManager::Pending TimerManager::pending() {
    std::lock_guard<std::mutex> lk(mtx_);
    Pending p;
    for (auto& [id, e] : entries_) {
        if (e.cancelled) continue;
        if (e.interval_ms > 0) ++p.repeating; else ++p.once;
    }
    return p;
}

bool TimerManager::active() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_cbs_ > 0) return true;
    for (auto& [id, e] : entries_) if (!e.cancelled) return true;
    return false;
}

// Channel deadlock (parallel_runtime.h): while a timer is installed "everyone is waiting" does not hold.
[[maybe_unused]] static const bool s_timers_hook_set =
    (g_timers_active = [] { return TimerManager::exists() && TimerManager::instance().active(); }, true);

bool TimerManager::wait_one_shots(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    auto idle = [&] {
        if (running_cbs_ > 0) return false;
        for (auto& [id, e] : entries_)
            if (!e.cancelled && e.interval_ms == 0) return false;
        return true;
    };
    if (timeout_ms < 0) { idle_cv_.wait(lk, idle); return true; }
    return idle_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), idle);
}

void TimerManager::shutdown() {
    // The flag is written UNDER mtx_. OLD BUG (lost wake-up): when written without the lock, a notify arriving while the thread
    // had evaluated the predicate (running_ still true) but not yet gone to sleep was lost,
    // the thread slept until the deadline (30 s when idle) and the join at exit kept the
    // process waiting that long -- measured: ~5% of scripts that touched a timer hung at exit.
    { std::lock_guard<std::mutex> lk(mtx_); running_.store(false); }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TimerManager::run() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lk(mtx_);

        auto now = std::chrono::steady_clock::now();
        auto deadline = now + std::chrono::seconds(30);
        for (auto& [id, e] : entries_) {
            if (!e.cancelled && e.next_fire < deadline)
                deadline = e.next_fire;
        }

        // 1.0.8: `deadline` is computed BEFORE entering the wait. A timer added (or
        // cancelled) while waiting woke the cv but the predicate said "not due yet"
        // and it went back to sleep until the OLD deadline (30 s when idle) -> a timer
        // installed while the manager was idle ran up to 30 seconds late. Only the FIRST timer in the process,
        // added before the thread had entered the wait, ran on time.
        // Wake when the list changes (dirty_) and recompute the deadline.
        dirty_ = false;
        cv_.wait_until(lk, deadline, [&] {
            if (!running_.load() || dirty_) return true;
            auto t = std::chrono::steady_clock::now();
            for (auto& [id, e] : entries_)
                if (!e.cancelled && e.next_fire <= t) return true;
            return false;
        });

        if (!running_.load()) break;

        now = std::chrono::steady_clock::now();

        std::vector<std::function<void()>> cbs;
        std::vector<bool> once;   // is cbs[i] a one-shot (timer::wait waits for it)
        std::vector<int> to_remove;

        for (auto& [id, e] : entries_) {
            if (e.cancelled) { to_remove.push_back(id); continue; }
            if (e.next_fire <= now) {
                cbs.push_back(e.callback);
                once.push_back(e.interval_ms == 0);
                if (e.interval_ms > 0)
                    e.next_fire = now + std::chrono::milliseconds(e.interval_ms);
                else {
                    to_remove.push_back(id);
                    ++running_cbs_;   // left the list but its callback has not finished yet
                }
            }
        }
        for (int id : to_remove) entries_.erase(id);

        lk.unlock();

        for (size_t i = 0; i < cbs.size(); ++i) {
            try { cbs[i](); } catch (...) {}
            if (once[i]) {
                { std::lock_guard<std::mutex> g(mtx_); --running_cbs_; }
                idle_cv_.notify_all();
            }
        }
    }
}

} // namespace look

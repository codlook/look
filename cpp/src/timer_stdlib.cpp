#include "look/timer.h"

namespace look {

TimerManager& TimerManager::instance() {
    static TimerManager inst;
    return inst;
}

TimerManager::TimerManager() {
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
    }
    cv_.notify_one();
    return id;
}

void TimerManager::cancel(int id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_.erase(id);   // Hemen sil — callback lambda ($ws shared_ptr) serbest kalır
    }
    cv_.notify_one();         // Thread'i uyandır — deadline güncellensin
}

void TimerManager::shutdown() {
    running_.store(false);
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

        cv_.wait_until(lk, deadline, [&] {
            if (!running_.load()) return true;
            auto t = std::chrono::steady_clock::now();
            for (auto& [id, e] : entries_)
                if (!e.cancelled && e.next_fire <= t) return true;
            return false;
        });

        if (!running_.load()) break;

        now = std::chrono::steady_clock::now();

        std::vector<std::function<void()>> cbs;
        std::vector<int> to_remove;

        for (auto& [id, e] : entries_) {
            if (e.cancelled) { to_remove.push_back(id); continue; }
            if (e.next_fire <= now) {
                cbs.push_back(e.callback);
                if (e.interval_ms > 0)
                    e.next_fire = now + std::chrono::milliseconds(e.interval_ms);
                else
                    to_remove.push_back(id);
            }
        }
        for (int id : to_remove) entries_.erase(id);

        lk.unlock();

        for (auto& cb : cbs) {
            try { cb(); } catch (...) {}
        }
    }
}

} // namespace look

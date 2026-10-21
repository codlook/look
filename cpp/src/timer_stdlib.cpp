#include "look/timer.h"

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
        entries_.erase(id);   // Hemen sil — callback lambda ($ws shared_ptr) serbest kalır
        dirty_ = true;
    }
    cv_.notify_one();         // Thread'i uyandır — deadline güncellensin
    idle_cv_.notify_all();    // timer::wait iptal edilen tek seferliği beklemesin
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
    // Bayrak mtx_ ALTINDA yazılır. ESKİ HATA (kaybolan uyandırma): kilitsiz yazılınca, iş parçacığı
    // yüklemi (running_ hâlâ true) değerlendirip henüz uykuya geçmemişken gelen notify kayboluyor,
    // iş parçacığı deadline'a (boşken 30 sn) kadar uyuyor ve çıkıştaki join süreci o kadar
    // bekletiyordu — ölçüm: zamanlayıcıya dokunan betiklerin ~%5'i çıkışta asılıyordu.
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

        // 1.0.8: `deadline` beklemeye girmeden ÖNCE hesaplanır. Beklerken eklenen (ya da iptal
        // edilen) bir zamanlayıcı cv'yi uyandırıyordu ama yüklem "henüz vakti gelmedi" deyip
        // ESKİ deadline'a (boşken 30 sn) kadar yeniden uyuyordu → yönetici boştayken kurulan
        // zamanlayıcı 30 saniyeye kadar geç çalışıyordu. Yalnız süreçteki İLK zamanlayıcı,
        // iş parçacığı henüz beklemeye girmeden eklendiği için zamanında çalışıyordu.
        // Liste değişince (dirty_) uyan ve deadline'ı yeniden hesapla.
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
        std::vector<bool> once;   // cbs[i] tek seferlik mi (timer::wait onu bekler)
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
                    ++running_cbs_;   // listeden çıktı ama geri çağırması henüz bitmedi
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

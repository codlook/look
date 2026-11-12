#include "look/parallel_runtime.h"
#include "look/fiber.h"
#include "look/value.h"   // LookChannel (kilitlenmede uyandırma)
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <vector>

namespace look {

// Senkronizasyon primitifleri KASITLI leak edilir (asla yok edilmez). parallel()
// task'ları detached (vm.cpp: std::thread(...).detach()). main()
// task_wait ile drain etse de iki delik kalır: (1) task_wait timeout'unu aşan task
// TERK edilir → çıkışta hâlâ çalışırken statikler yok edilir; (2) son task_release()
// s_cv.notify_all()/s_mtx.lock içindeyken, waiter predicate (s_count<=0) fetch_sub'dan
// hemen sonra true olup uyanır → main çıkar → static destructor pthread_cond_destroy'u
// notify ile YARIŞTIRIR (TSan: "Location is global look::s_cv"). Heap'te leak → destructor
// hiç çalışmaz, terk edilen task süreç sonuna dek güvenle dokunur; OS belleği süreçle alır.
// (Abseil/Google global mutex idiomu — süreç-ömürlü singleton'ı çıkışta yok etme.)
#ifdef LOOK_PR_STATIC_TEST   // yalnız TSan pozitif kontrol: eski (yarışlı) statik hâl
static std::atomic<int>        s_count{0};
static std::mutex              s_mtx;
static std::condition_variable s_cv;
#else
static std::atomic<int>&        s_count = *new std::atomic<int>(0);
static std::mutex&              s_mtx   = *new std::mutex();
static std::condition_variable& s_cv    = *new std::condition_variable();
#endif

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
                    "parallel(): backpressure timeout — waiting for a task slot (" +
                    std::to_string(timeout_ms) + " ms)");
            }
        }
    }

    // AcquireMode::THROW (default)
    int prev = s_count.fetch_add(1);
    if (prev >= task_limit()) {
        s_count.fetch_sub(1);
        throw std::runtime_error(
            "parallel(): task limit exceeded (" +
            std::to_string(task_limit()) + "). " +
            "can be raised with the LOOK_PARALLEL_LIMIT env var.");
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

// ── Betik sonu (parallel_runtime.h) ── kayıt da KASITLI sızdırılır (yukarıdaki gerekçe).
static std::mutex&                     s_exit_mtx = *new std::mutex();
static std::condition_variable&        s_exit_cv  = *new std::condition_variable();
static std::condition_variable&        s_park_cv  = *new std::condition_variable();  // hiç uyandırılmaz
static std::vector<TaskExit*>&         s_exit_reg = *new std::vector<TaskExit*>();
std::atomic<bool> g_task_script_ending{false};   // betik sonu (main.cpp finish_script)
// fiber-ok: parallel() görevinin kendi iş parçacığı (fiber değil); istek verisi değil
thread_local TaskExit* t_task_exit = nullptr;

static std::atomic<int> s_live_tasks{0};   // kayıtlı, bitmemiş görev (kanal kilitlenmesi sayımı)
static void chan_deadlock_recheck() noexcept;

TaskExit* task_exit_new() {
    auto* t = new TaskExit();
    s_live_tasks.fetch_add(1);
    std::lock_guard<std::mutex> lk(s_exit_mtx);
    s_exit_reg.push_back(t);
    return t;
}

void task_exit_enter(TaskExit* t) noexcept { t_task_exit = t; }

void task_exit_done(TaskExit* t) noexcept {
    t_task_exit = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_exit_mtx);
        t->phase.store(TASK_DONE);
        for (size_t i = 0; i < s_exit_reg.size(); ++i)
            if (s_exit_reg[i] == t) { s_exit_reg[i] = s_exit_reg.back(); s_exit_reg.pop_back(); break; }
        s_exit_cv.notify_all();
    }
    delete t;
    s_live_tasks.fetch_sub(1);
    chan_deadlock_recheck();   // betiğin beklediği son görev göndermeden bittiyse
}


void task_stop_here(TaskExit* t) noexcept {
    std::unique_lock<std::mutex> lk(s_exit_mtx);
    t->phase.store(TASK_STOPPED);
    s_exit_cv.notify_all();
    for (;;) s_park_cv.wait(lk);   // süreç _Exit ile biter; bu iş parçacığı bir daha ilerlemez
}

bool task_point_slow(TaskExit* t, bool effect) noexcept {
    if (g_task_script_ending.load()) {
        if (effect || --t->after_budget <= 0) task_stop_here(t);
        return false;
    }
    if (!effect) return false;
    // Dekker: önce durumu yaz, sonra işarete bak (ikisi de seq_cst). Ana iş parçacığı işareti
    // yazıp durumu okur → biri ötekini mutlaka görür.
    t->phase.store(TASK_IN_EFFECT);
    if (g_task_script_ending.load()) task_stop_here(t);
    return true;
}

// fiber.h kancası: preempt yavaş yolu (her 1024 noktada bir) betik sonunda görevi sınırlar.
[[maybe_unused]] static const bool s_preempt_hook_set = (g_preempt_hook = &task_preempt_check, true);

void task_preempt_check() noexcept {
    if (TaskExit* t = t_task_exit; t && g_task_script_ending.load()) task_point_slow(t, false);
}

// ── Kanal kilitlenmesi (parallel_runtime.h) ── kayıt da sızdırılır (yukarıdaki gerekçe).
struct DlWaiter { ChanWait* w; std::shared_ptr<LookChannel> ch; int kind; uint64_t gen; };
static std::mutex&            s_dl_mtx     = *new std::mutex();
static std::vector<DlWaiter>& s_dl_waiting = *new std::vector<DlWaiter>();
static bool     s_dl_main  = false;  // s_dl_mtx altında: komut satırı betiği çalışıyor (ana iş parçacığı canlı)
static uint64_t s_dl_epoch = 0;      // s_dl_mtx altında: her giriş/çıkış/görev bitişinde artar
std::atomic<bool> g_chan_deadlock{false};
TimersActiveFn    g_timers_active = nullptr;

const char* chan_deadlock_message() noexcept {
    return "deadlock: the script and every parallel() task are waiting on a channel, and nothing "
           "is left that could send or receive";
}

// s_dl_mtx tutulurken: sayılar kilitlenme gösteriyor mu (şüphe)?
static bool dl_suspect_locked() {
    return s_dl_main && !g_chan_deadlock.load() &&
           (int)s_dl_waiting.size() == 1 + s_live_tasks.load();
}

ChanWait::ChanWait(std::shared_ptr<LookChannel> c, int kind, uint64_t gen) : ch(std::move(c)) {
    std::lock_guard<std::mutex> lk(s_dl_mtx);
    s_dl_waiting.push_back({this, ch, kind, gen});
    ++s_dl_epoch;
    suspect = dl_suspect_locked();
}

ChanWait::~ChanWait() {
    std::lock_guard<std::mutex> lk(s_dl_mtx);
    for (size_t i = 0; i < s_dl_waiting.size(); ++i)
        if (s_dl_waiting[i].w == this) { s_dl_waiting[i] = s_dl_waiting.back(); s_dl_waiting.pop_back(); break; }
    ++s_dl_epoch;
}

// Bekleyen artık ilerleyebilir mi (kanalın kilidiyle okunur).
static bool dl_waiter_ready(const DlWaiter& d) {
    std::lock_guard<std::mutex> lk(d.ch->mtx);
    if (d.ch->closed) return true;
    switch (d.kind) {
        case CHAN_WAIT_RECV:  return !d.ch->queue.empty();
        case CHAN_WAIT_ROOM:  return d.ch->queue.size() < d.ch->capacity;
        default:              return d.ch->recv_gen > d.gen;
    }
}

void chan_deadlock_try() noexcept {
    std::vector<DlWaiter> snap;
    uint64_t epoch;
    {
        std::lock_guard<std::mutex> lk(s_dl_mtx);
        if (!dl_suspect_locked()) return;
        snap = s_dl_waiting;
        epoch = s_dl_epoch;
    }
    if (g_timers_active && g_timers_active()) return;      // geri çağırma gönderebilir
    for (const auto& d : snap) if (dl_waiter_ready(d)) return;   // biri ilerleyebiliyor
    {
        std::lock_guard<std::mutex> lk(s_dl_mtx);
        if (s_dl_epoch != epoch || !dl_suspect_locked()) return;   // arada değişti: değiştiren yineler
        g_chan_deadlock.store(true);
    }
    for (const auto& d : snap) {   // kaybolan uyandırma olmasın: kilitle, sonra bildir
        std::lock_guard<std::mutex> lk(d.ch->mtx);
        d.ch->not_empty.notify_all();
        d.ch->not_full.notify_all();
    }
}

static void chan_deadlock_recheck() noexcept {
    { std::lock_guard<std::mutex> lk(s_dl_mtx); ++s_dl_epoch; }
    chan_deadlock_try();
}

void chan_deadlock_main(bool on) noexcept {
    std::lock_guard<std::mutex> lk(s_dl_mtx);
    s_dl_main = on;
    ++s_dl_epoch;
}

int task_settle_for_exit() {
    std::unique_lock<std::mutex> lk(s_exit_mtx);
    g_task_script_ending.store(true);
    s_exit_cv.wait(lk, [] {
        for (TaskExit* t : s_exit_reg) if (t->phase.load() == TASK_RUNNING) return false;
        return true;
    });
    return (int)s_exit_reg.size();
}

} // namespace look

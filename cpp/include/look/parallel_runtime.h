#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace look {

// ── Parallel task lifecycle ───────────────────────────────────────────────────
// Default limit: 64.  Override with env var LOOK_PARALLEL_LIMIT (integer).
// Small server: 32.  CPU-bound batch workers: 128.
// Note: SMTP / HTTP listeners are I/O-bound and must use event_loop, not parallel().

// Returns the active task limit (reads LOOK_PARALLEL_LIMIT once, then caches).
int task_limit();

// Acquire modes for task_acquire():
//   THROW — throws std::runtime_error when limit is reached (default, fast fail)
//   WAIT  — blocks until a slot is free (real backpressure, honours timeout_ms)
//   TRY   — returns false immediately if no slot available (caller decides)
enum class AcquireMode { THROW, WAIT, TRY };

// Reserve a task slot.
//   THROW: throws on limit. WAIT: blocks up to timeout_ms. TRY: returns false on limit.
// Returns true if acquired, false only in TRY mode when limit is full.
bool task_acquire(AcquireMode mode = AcquireMode::THROW, int timeout_ms = 0);

// Release a task slot and signal waiters.  Always pair with a successful acquire.
// Prefer TaskGuard — it calls this automatically.
void task_release() noexcept;

// Active task count (instantaneous snapshot).
int  task_active();

// Block until all tasks finish or timeout_ms elapses.
// Returns true if count reached 0, false on timeout.
bool task_wait(int timeout_ms = 5000);

// RAII guard: calls task_release() on scope exit regardless of exceptions.
struct TaskGuard {
    ~TaskGuard() { task_release(); }
};

// ── Betik sonu: görevin işi kalmış mı? (main.cpp finish_script) ───────────────
// Betik bitince çalışan görev, ya DÖNDÜĞÜ ya da dışarıda iz bırakan bir iş yapmaya başladığı
// İLK ana kadar ilerler; zamana bakılmaz, betik görevi beklemez. "İz bırakan" = yerleşiğin
// etki sınıfı Effect (çıktı, kanal, dosya, ağ, veritabanı) ya da sınıfsız; Pure/Reads/
// ThrowCheck/Callback çalışmaya devam eder (count(), string::upper…). Saf hesap da sınırlı:
// betik bittikten sonra 16 saf yerleşik ya da 16 preempt yavaş yolu (≈16k geriye atlama/
// çağrı) → durdurulur (sonsuz döngü beklenmez). Eskiden send()'den sonra yalnız dönüşü kalan
// görev de "still running" sayılıyordu: tek send, döngü, if dalı — 50/50 yanlış uyarı.
// Kanal: send değeri TESLİM etmeden hemen önce görevi RUNNING'e alır (tamponsuzda alıcı alır).
enum : int { TASK_RUNNING = 0, TASK_IN_EFFECT = 1, TASK_DONE = 2, TASK_STOPPED = 3 };
struct TaskExit {
    std::atomic<int> phase{TASK_RUNNING};
    int after_budget = 16;   // yalnız görevin kendi iş parçacığı yazar
};
TaskExit* task_exit_new();                 // PARALLEL_CALL'da (iş parçacığı başlamadan) kaydedilir
void task_exit_enter(TaskExit* t) noexcept; // görevin iş parçacığı başında (t_task_exit)
void task_exit_done(TaskExit* t) noexcept;  // görev bitti → kayıttan çık, sil, t_task_exit = null
extern std::atomic<bool> g_task_script_ending;
inline bool task_script_ending() noexcept { return g_task_script_ending.load(); }
[[noreturn]] void task_stop_here(TaskExit* t) noexcept;
bool task_point_slow(TaskExit* t, bool effect) noexcept;
// Yerleşik/kanal noktası. effect ise IN_EFFECT'e geçer ve true döner (sonra task_effect_end).
// Hızlı yol satır içi: betik sürerken saf yerleşik = tek okuma (görevde +%1,1 ölçüldü → ~0).
inline bool task_point(TaskExit* t, bool effect) noexcept {
    if (!effect && !g_task_script_ending.load(std::memory_order_relaxed)) return false;
    return task_point_slow(t, effect);
}
inline void task_effect_end(TaskExit* t) noexcept { t->phase.store(TASK_RUNNING); }
void task_preempt_check() noexcept;        // preempt_slow'dan (her 1024 noktada bir)
// Betik sonu: bitiyor işaretini kaldır, RUNNING görev kalmayana dek bekle. İşi kalan görev sayısı.
int task_settle_for_exit();
// fiber-ok: iş parçacığının kendi görev kaydı (parallel() iş parçacığı), istek verisi değil
extern thread_local TaskExit* t_task_exit;

} // namespace look

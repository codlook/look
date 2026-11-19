#include "look/parallel_runtime.h"
#include "look/fiber.h"
#include "look/value.h"   // LookChannel (wake-up on deadlock)
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <vector>

namespace look {

// Synchronization primitives are DELIBERATELY leaked (never destroyed). parallel()
// tasks are detached (vm.cpp: std::thread(...).detach()). Even though main()
// drains with task_wait, two holes remain: (1) a task that exceeds the task_wait timeout is
// ABANDONED -> the statics are destroyed at exit while it is still running; (2) the last task_release()
// is still inside s_cv.notify_all()/s_mtx.lock when the waiter predicate (s_count<=0) becomes true
// right after fetch_sub and wakes up -> main exits -> the static destructor RACES pthread_cond_destroy
// with notify (TSan: "Location is global look::s_cv"). Leaked on the heap -> the destructor
// never runs, the abandoned task can safely touch it until the process ends; the OS reclaims the memory with the process.
// (Abseil/Google global mutex idiom — do not destroy process-lifetime singletons at exit.)
#ifdef LOOK_PR_STATIC_TEST   // TSan positive control only: the old (racy) static form
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

// ── Script end (parallel_runtime.h) ── the registry is also DELIBERATELY leaked (reason above).
static std::mutex&                     s_exit_mtx = *new std::mutex();
static std::condition_variable&        s_exit_cv  = *new std::condition_variable();
static std::condition_variable&        s_park_cv  = *new std::condition_variable();  // never woken up
static std::vector<TaskExit*>&         s_exit_reg = *new std::vector<TaskExit*>();
std::atomic<bool> g_task_script_ending{false};   // script end (main.cpp finish_script)
// fiber-ok: the parallel() task's own thread (not a fiber); not request data
thread_local TaskExit* t_task_exit = nullptr;

static std::atomic<int> s_live_tasks{0};   // registered, unfinished task (channel deadlock count)
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
    chan_deadlock_recheck();   // if the last task the script is waiting for finished without sending
}


void task_stop_here(TaskExit* t) noexcept {
    std::unique_lock<std::mutex> lk(s_exit_mtx);
    t->phase.store(TASK_STOPPED);
    s_exit_cv.notify_all();
    for (;;) s_park_cv.wait(lk);   // the process ends with _Exit; this thread never makes progress again
}

bool task_point_slow(TaskExit* t, bool effect) noexcept {
    if (g_task_script_ending.load()) {
        if (effect || --t->after_budget <= 0) task_stop_here(t);
        return false;
    }
    if (!effect) return false;
    // Dekker: write the state first, then look at the flag (both seq_cst). The main thread writes the flag
    // and reads the state -> one of them is guaranteed to see the other.
    t->phase.store(TASK_IN_EFFECT);
    if (g_task_script_ending.load()) task_stop_here(t);
    return true;
}

// fiber.h hook: the preempt slow path (once every 1024 points) bounds the task at script end.
[[maybe_unused]] static const bool s_preempt_hook_set = (g_preempt_hook = &task_preempt_check, true);

void task_preempt_check() noexcept {
    if (TaskExit* t = t_task_exit; t && g_task_script_ending.load()) task_point_slow(t, false);
}

// ── Channel deadlock (parallel_runtime.h) ── the registry is also leaked (reason above).
struct DlWaiter { ChanWait* w; std::shared_ptr<LookChannel> ch; int kind; uint64_t gen; };
static std::mutex&            s_dl_mtx     = *new std::mutex();
static std::vector<DlWaiter>& s_dl_waiting = *new std::vector<DlWaiter>();
static bool     s_dl_main  = false;  // under s_dl_mtx: the command-line script is running (main thread alive)
static uint64_t s_dl_epoch = 0;      // under s_dl_mtx: incremented on every entry/exit/task finish
std::atomic<bool> g_chan_deadlock{false};
TimersActiveFn    g_timers_active = nullptr;

const char* chan_deadlock_message() noexcept {
    return "deadlock: the script and every parallel() task are waiting on a channel, and nothing "
           "is left that could send or receive";
}

// while holding s_dl_mtx: do the counts indicate a deadlock (suspicion)?
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

// Can the waiter make progress now (read under the channel's lock)?
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
    if (g_timers_active && g_timers_active()) return;      // may deliver a callback
    for (const auto& d : snap) if (dl_waiter_ready(d)) return;   // one of them can make progress
    {
        std::lock_guard<std::mutex> lk(s_dl_mtx);
        if (s_dl_epoch != epoch || !dl_suspect_locked()) return;   // changed in the meantime: the one that changed it iterates again
        g_chan_deadlock.store(true);
    }
    for (const auto& d : snap) {   // no lost wake-up: lock, then notify
        std::lock_guard<std::mutex> lk(d.ch->mtx);
        d.ch->not_empty.notify_all();
        d.ch->not_full.notify_all();
        LookChannel::wake_fibers(d.ch->fiber_recv);   // so that waiters on fibers also see the error
        LookChannel::wake_fibers(d.ch->fiber_send);
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

// pr_shutdown_tsan.cpp — positive-control harness for the parallel_runtime shutdown race.
//
// s_cv/s_mtx are file-static → destroyed at program exit. parallel() tasks are
// detached; main drains with task_wait(timeout) but a task that exceeds the timeout is ABANDONED.
// When the abandoned task wakes and calls task_release(), the statics may already have been
// destroyed → a pthread_cond_destroy vs notify RACE (TSan: global look::s_cv).
//
// TSan must see a race in the BUGGY (static) build; the FIXED (leak) build must be CLEAN.
//   Positive control: PRODUCE the race, prove that the fix removes the race.
#include "look/parallel_runtime.h"
#include <thread>
#include <chrono>

int main() {
    // a task that EXCEEDS the task_wait deadline: main abandons it and exits, the statics are destroyed,
    // then the task wakes and touches s_cv via task_release().
    look::task_acquire(); // THROW mode, take a slot
    std::thread([]{
        look::TaskGuard _g; // task_release() at scope exit → s_cv.notify
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }).detach();

    // main: wait with a short timeout → the task takes 300ms, we give up at 50ms →
    // the task is ABANDONED, main returns → the static destructor destroys s_cv.
    look::task_wait(50);
    return 0; // ← s_cv/s_mtx are destroyed here; the abandoned task is still sleeping
}

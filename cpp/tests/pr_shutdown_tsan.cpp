// pr_shutdown_tsan.cpp — parallel_runtime shutdown yarışı POZİTİF KONTROL harness'ı.
//
// s_cv/s_mtx dosya-statik → program çıkışında yok edilir. parallel() task'ları
// detached; main task_wait(timeout) ile drain eder ama timeout'u aşan task TERK
// edilir. Terk edilen task uyanıp task_release() çağırınca, statikler çoktan yok
// edilmiş olabilir → pthread_cond_destroy vs notify YARIŞI (TSan: global look::s_cv).
//
// BUG'lu (statik) derlemede TSan yarış görmeli; FIX'li (leak) derlemede TEMİZ.
//   Pozitif kontrol: yarışı ÜRET, fix'in yarışı GİDERDİĞİNİ kanıtla.
#include "look/parallel_runtime.h"
#include <thread>
#include <chrono>

int main() {
    // task_wait deadline'ını AŞAN bir task: main terk edip çıkacak, statikler yok
    // edilecek, sonra task uyanıp task_release() ile s_cv'ye dokunacak.
    look::task_acquire(); // THROW mode, slot al
    std::thread([]{
        look::TaskGuard _g; // scope-exit'te task_release() → s_cv.notify
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }).detach();

    // main: kısa timeout ile bekle → task 300ms'de, biz 50ms'de vazgeçiyoruz →
    // task TERK edilir, main return → static destructor s_cv'yi yok eder.
    look::task_wait(50);
    return 0; // ← burada s_cv/s_mtx yok edilir; terk edilen task hâlâ uyuyor
}

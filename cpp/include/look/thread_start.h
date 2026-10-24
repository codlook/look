#pragma once
// Bir havuzun iş parçacıklarını başlat — biri başlatılamazsa (bellek, ulimit -u, ulimit -v)
// başlatılanları DURDUR ve BİRLEŞTİR, sonra nedeni yazan bir istisna fırlat.
//
// Eskiden kurucu emplace_back'in istisnasıyla yarıda kalıyordu: başlamış iş parçacıkları
// koşul değişkeninde beklerken onu yok etmek glibc 2.28'de SONSUZA DEK takılıyordu (süreç ayakta,
// port açık değil, log yok), takılmazsa joinable std::thread → std::terminate. Go'da olduğu gibi:
// iş parçacığı açılamıyorsa program nedenini söyler ve hata koduyla çıkar (bkz. main).
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace look {

template <class Body, class StopAll>
inline void start_pool_threads(std::vector<std::thread>& threads, int n, const char* what,
                               Body body, StopAll stop_all) {
    threads.reserve(n);
    for (int i = 0; i < n; ++i) {
        try {
            threads.emplace_back(body);
        } catch (const std::system_error& e) {
            stop_all();
            for (auto& t : threads) if (t.joinable()) t.join();
            threads.clear();
            throw std::runtime_error(std::string("could not start ") + what + " thread " +
                std::to_string(i + 1) + " of " + std::to_string(n) + " (" + e.what() +
                "). Use fewer threads (--workers, LOOK_DB_POOL_SIZE) or raise the process's limits "
                "(ulimit -u, ulimit -v).");
        }
    }
}

} // namespace look

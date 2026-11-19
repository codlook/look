#pragma once
// Start a pool's threads -- if one cannot be started (memory, ulimit -u, ulimit -v)
// STOP and JOIN the ones already started, then throw an exception stating the reason.
//
// The constructor used to be left half-done by emplace_back's exception: destroying started threads
// while they waited on a condition variable hung FOREVER on glibc 2.28 (process alive,
// port not open, no log); if it did not hang, a joinable std::thread -> std::terminate. As in Go:
// if a thread cannot be created the program states the reason and exits with an error code (see main).
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

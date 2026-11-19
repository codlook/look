// Graceful shutdown — look/shutdown.h
#include "look/shutdown.h"
#include "look/logger.h"
#include "look/web.h"   // clear_db_pools
#include "look/websocket.h"   // g_ws_registry — 1001 at shutdown
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#ifndef _WIN32
#  include <csignal>
#  include <pthread.h>
#  include <unistd.h>
#endif

namespace look::shutdown {

namespace {
std::atomic<bool> g_requested{false};
std::mutex        g_mtx;
std::function<void()> g_begin;   // under g_mtx
constexpr int     kDrainSeconds = 30;

void log_line(LogLevel lv, const std::string& msg) {
    Logger::instance().log(lv, "HTTP", msg);
}
} // namespace

bool requested() { return g_requested.load(std::memory_order_acquire); }

void on_begin(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_begin = std::move(fn);
}

[[noreturn]] void finish(int code) {
    // Open WebSockets get 1001 Going Away (requests finished or time ran out; the process is exiting).
    try { g_ws_registry.going_away_all(); } catch (...) {}
    try { clear_db_pools(); } catch (...) {}   // pool destructors close connections cleanly (MySQL COM_QUIT)
    log_line(code == 0 ? LogLevel::LOG_INFO : LogLevel::LOG_WARN,
             code == 0 ? "stopped" : "stopped before every request finished");
    std::fflush(nullptr);
    std::_Exit(code);
}

#ifdef _WIN32
void install() {}
#else
void install() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    // This thread and EVERY thread opened later do not receive these signals; only the waiter below does.
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set]() {
        int sig = 0;
        while (sigwait(&set, &sig) != 0) {}
        const char* name = sig == SIGINT ? "SIGINT" : "SIGTERM";
        g_requested.store(true, std::memory_order_release);
        std::function<void()> begin;
        { std::lock_guard<std::mutex> lk(g_mtx); begin = g_begin; }
        if (!begin) {
            // The server is not set up yet (setup in progress): no request being processed.
            log_line(LogLevel::LOG_INFO, std::string(name) + ": stopping (the application was still starting)");
            finish(0);
        }
        log_line(LogLevel::LOG_INFO, std::string(name) + ": no new connections; finishing the requests in progress "
                 "(at most " + std::to_string(kDrainSeconds) + " s; a second signal stops at once)");
        // Time limit and second signal: a separate waiter. When the main flow ends finish() terminates the process anyway.
        std::thread([set]() {
            struct timespec ts{1, 0};
            for (int i = 0; i < kDrainSeconds; ++i) {
                siginfo_t info;
                if (sigtimedwait(&set, &info, &ts) > 0) {
                    log_line(LogLevel::LOG_WARN, "second signal: stopping now");
                    finish(1);
                }
            }
            log_line(LogLevel::LOG_WARN, "requests still running after " + std::to_string(kDrainSeconds) +
                     " s; stopping without them");
            finish(1);
        }).detach();
        begin();
    }).detach();
}
#endif

} // namespace look::shutdown

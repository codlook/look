// Zarif kapanma — look/shutdown.h
#include "look/shutdown.h"
#include "look/logger.h"
#include "look/web.h"   // clear_db_pools
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
std::function<void()> g_begin;   // g_mtx altında
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
    try { clear_db_pools(); } catch (...) {}   // havuz yıkıcıları bağlantıları düzgün kapatır (MySQL COM_QUIT)
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
    // Bu iş parçacığı ve sonra açılan HER iş parçacığı bu sinyalleri almaz; yalnız aşağıdaki bekler.
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set]() {
        int sig = 0;
        while (sigwait(&set, &sig) != 0) {}
        const char* name = sig == SIGINT ? "SIGINT" : "SIGTERM";
        g_requested.store(true, std::memory_order_release);
        std::function<void()> begin;
        { std::lock_guard<std::mutex> lk(g_mtx); begin = g_begin; }
        if (!begin) {
            // Sunucu henüz kurulmadı (kurulum sürüyor): işlenen istek yok.
            log_line(LogLevel::LOG_INFO, std::string(name) + ": stopping (the application was still starting)");
            finish(0);
        }
        log_line(LogLevel::LOG_INFO, std::string(name) + ": no new connections; finishing the requests in progress "
                 "(at most " + std::to_string(kDrainSeconds) + " s; a second signal stops at once)");
        // Süre sınırı ve ikinci sinyal: ayrı bekleyici. Ana akış bitince finish() süreci zaten sonlandırır.
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

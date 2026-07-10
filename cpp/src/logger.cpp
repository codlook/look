#include "look/logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <direct.h>   // _mkdir
#include <windows.h>  // CreateFile, FILE_SHARE_WRITE
#define MAKE_DIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MAKE_DIR(p) mkdir(p, 0755)
#endif

// Windows ERROR/DEBUG makro çakışmasını önle
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif

namespace look {

struct LoggerState {
    std::string log_dir   = "logs";
    bool        verbose   = false;
    LogLevel    min_level = LogLevel::LOG_INFO;
    std::mutex  mtx;

    LoggerState() {
        // Başlangıçta system env'den oku
        const char* app_env = std::getenv("APP_ENV");
        const char* debug   = std::getenv("APP_DEBUG");
        const char* log_d   = std::getenv("LOG_DIR");

        if (log_d) log_dir = log_d;

        bool is_dev = (app_env && std::string(app_env) == "development");
        bool is_dbg = (debug   && std::string(debug)   == "true");

        verbose   = is_dev || is_dbg;
        min_level = (is_dev || is_dbg) ? LogLevel::LOG_DEBUG : LogLevel::LOG_INFO;
    }
};

static LoggerState& state() {
    static LoggerState s;
    return s;
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::configure(const std::string& log_dir, bool verbose, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(state().mtx);
    state().log_dir   = log_dir;
    state().verbose   = verbose;
    state().min_level = min_level;
}

static std::string timestamp_str() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;
    struct tm tm_buf = {};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time);
#else
    gmtime_r(&time, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

static std::string today_str() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf = {};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time);
#else
    gmtime_r(&time, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}

static const char* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::LOG_DEBUG: return "DEBUG";
        case LogLevel::LOG_INFO:  return "INFO ";
        case LogLevel::LOG_WARN:  return "WARN ";
        case LogLevel::LOG_ERROR: return "ERROR";
    }
    return "?????";
}

void Logger::log(LogLevel level, const std::string& category, const std::string& message) {
    auto& s = state();
    if (level < s.min_level) return;

    // Log injection koruması: \r \n ve kontrol karakterleri kaldır
    std::string safe_msg;
    safe_msg.reserve(message.size());
    for (unsigned char c : message)
        if (c >= 0x20 || c == '\t') safe_msg += c;  // tab dışındaki kontrol karakterlerini at
        else safe_msg += ' ';
    std::string line = "[" + timestamp_str() + "] [" + level_str(level) + "] [" + category + "] " + safe_msg;

    std::lock_guard<std::mutex> lock(s.mtx);

    // logs/ klasörünü oluştur
    MAKE_DIR(s.log_dir.c_str());

    // Dosyaya yaz — Windows: FILE_SHARE_READ|FILE_SHARE_WRITE ile
    // 1000 eşzamanlı process aynı anda yazabilsin (exclusive lock yok)
    std::string path = s.log_dir + "/look-" + today_str() + ".log";
#ifdef _WIN32
    // Dizini backslash ile olustur — tum parent'lar dahil (recursive)
    std::string win_dir = s.log_dir;
    for (char& c : win_dir) if (c == '/') c = '\\';
    // Her seviyeyi tek tek olustur
    for (size_t i = 1; i < win_dir.size(); ++i) {
        if (win_dir[i] == '\\') {
            std::string sub = win_dir.substr(0, i);
            CreateDirectoryA(sub.c_str(), NULL);
        }
    }
    CreateDirectoryA(win_dir.c_str(), NULL); // son seviye + ERROR_ALREADY_EXISTS tamam

    std::string win_path = path;
    for (char& c : win_path) if (c == '/') c = '\\';

    HANDLE hFile = CreateFileA(
        win_path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string out = line + "\n";
        DWORD written;
        WriteFile(hFile, out.c_str(), (DWORD)out.size(), &written, NULL);
        CloseHandle(hFile);
    } else {
        // Fallback: stderr'e yaz, hata kodunu da ekle
        std::cerr << "[LOGGER-ERR] CreateFileA failed for: " << win_path
                  << " err=" << GetLastError() << "\n";
    }
#else
    std::ofstream f(path, std::ios::app);
    if (f) f << line << "\n";
#endif

    // stderr'e her zaman yaz (Apache error.log'a düşer)
    std::cerr << line << "\n";
}

void Logger::log_query(const std::string& sql, long long ms) {
    log(LogLevel::LOG_DEBUG, "QUERY", "[" + std::to_string(ms) + "ms] " + sql);
}

} // namespace look

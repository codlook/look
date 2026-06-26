#pragma once
#include <string>

namespace look {

enum class LogLevel { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR };

class Logger {
public:
    static Logger& instance();
    void configure(const std::string& log_dir, bool verbose, LogLevel min_level);
    void log(LogLevel level, const std::string& category, const std::string& message);
    void log_query(const std::string& sql, long long ms);
};

} // namespace look

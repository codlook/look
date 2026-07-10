#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <shared_mutex>

namespace look {

// ── SSE connection — one per connected client ─────────────────────────────────

struct SseConnection {
    int fd;
    std::string client_ip;
    std::atomic<bool> closed{false};
    std::mutex write_mutex;
    std::function<void()> on_close_cb;

    explicit SseConnection(int fd_, std::string ip = "")
        : fd(fd_), client_ip(std::move(ip)) {}

    // Returns false if connection closed or write failed
    bool send(const std::string& data, const std::string& event_name = "");
    bool send_comment(const std::string& comment);
    void close_conn();
};

// ── SSE registry — tracks all active SSE connections ─────────────────────────

class SseRegistry {
    std::shared_mutex mutex_;
    std::unordered_map<int, std::shared_ptr<SseConnection>> clients_;
public:
    void add(std::shared_ptr<SseConnection> conn);
    void remove(int fd);
    size_t count();
};

extern SseRegistry g_sse_registry;

} // namespace look

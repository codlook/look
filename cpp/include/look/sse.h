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
    std::string held_ip;     // the LOOK_HTTP_MAX_CONNS_IP slot is held for this address (empty = none)
    std::atomic<bool> closed{false};
    std::mutex write_mutex;
    std::function<void()> on_close_cb;   // ONLY through the locked helpers (same race as WsConnection)
    std::mutex            cb_mtx;
    void set_on_close_cb(std::function<void()> f) { std::lock_guard<std::mutex> l(cb_mtx); on_close_cb = std::move(f); }
    std::function<void()> get_on_close_cb() { std::lock_guard<std::mutex> l(cb_mtx); return on_close_cb; }
    // A server-triggered close (sse::close) comes from a worker thread; fd/epoll/map cleanup
    // BELONGS to the loop thread -> this bridge hands close_sse over to it with loop->post (no race).
    // http_server sets it when the connection is established; with no loop (CLI) it is empty -> close_conn fallback.
    std::function<void()> request_loop_close;

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

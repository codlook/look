#pragma once

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <cstdint>

namespace look {

// ── WsConnection — single client handle ──────────────────────────────────────

struct WsConnection {
    int                  fd;
    std::string          client_ip;
    std::atomic<bool>    closed{false};
    std::mutex           write_mutex;

    // Callbacks — set by ws::on() from LOOK code
    std::function<void(const std::string&)> on_message;
    std::function<void()>                   on_close;

    explicit WsConnection(int fd_, std::string ip = "")
        : fd(fd_), client_ip(std::move(ip)) {}

    // Thread-safe: encode + send text frame
    bool send_text(const std::string& msg);

    // Send close frame and mark as closed
    void close_conn();

    // Raw send (for pong etc.) — caller must hold write_mutex
    bool send_raw(const std::string& frame);

    WsConnection(const WsConnection&)            = delete;
    WsConnection& operator=(const WsConnection&) = delete;
};

// ── Global client registry — thread-safe ─────────────────────────────────────

class WsRegistry {
    std::shared_mutex                                       mutex_;
    std::unordered_map<int, std::shared_ptr<WsConnection>> clients_;
public:
    void   add(std::shared_ptr<WsConnection> conn);
    void   remove(int fd);
    void   broadcast(const std::string& msg);
    size_t count();
};

extern WsRegistry g_ws_registry;

// ── RFC 6455 frame codec ──────────────────────────────────────────────────────

std::string ws_encode_text_frame (const std::string& payload);
std::string ws_encode_close_frame();
std::string ws_encode_pong_frame (const std::string& payload);

struct WsFrame {
    bool        fin      = true;
    uint8_t     opcode   = 0;     // 0x1=text, 0x2=bin, 0x8=close, 0x9=ping, 0xA=pong
    std::string payload;
    size_t      consumed = 0;
    bool        complete = false;
};

// Try to decode one frame from buf — returns complete=false if more data needed.
WsFrame ws_try_decode_frame(const std::string& buf);

// ── HTTP → WebSocket handshake (RFC 6455 §4) ─────────────────────────────────

std::string ws_accept_key(const std::string& client_key);
std::string ws_handshake_101(const std::string& client_key);

} // namespace look

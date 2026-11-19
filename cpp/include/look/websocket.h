#pragma once

#include <string>
#include <string_view>
#include <list>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <cstdint>

namespace look {

// ── WsConnection — single client handle ──────────────────────────────────────

// Hook that closes the fd after ws::close — installed by HttpServer (one server per process); adds no field
// per connection. Called with the fd; the server closes it if the connection on that fd is still server-closed.
extern void (*g_ws_server_close_hook)(int fd);

struct WsConnection {
    int                  fd;
    std::string          client_ip;
    std::string          held_ip;     // a LOOK_HTTP_MAX_CONNS_IP slot is held for this address (empty = none)
    std::atomic<bool>    closed{false};
    // "close" callback: ONCE when the client goes away (close frame, disconnect, send error);
    // never if the server closed with ws::close. `closed` alone did not distinguish: after the client left,
    // a failed ws::send set closed=true and the callback was skipped.
    std::atomic<bool>    server_closed{false};
    std::atomic<bool>    close_fired{false};
    bool                 fd_released = false;   // under write_mutex: close_ws closed the fd (can be reused)
    std::mutex           write_mutex;

    // Callbacks — set by ws::on() from LOOK code. ONLY through the locked helpers below: ws::on assigns from a
    // pool thread (also from inside a callback) while the event loop copies at the same time.
    // An unlocked std::function assignment + copy raced (ASan: heap-use-after-free, silent message loss).
    // on_message: (data, is_binary) — a binary frame (opcode 0x2) is distinguished from text (the second parameter
    // of the ws::on handler). Previously both were delivered as plain text.
    std::function<void(const std::string&, bool)> on_message;
    std::function<void()>                   on_close;
    std::mutex                              cb_mtx;
    void set_on_message(std::function<void(const std::string&, bool)> f) { std::lock_guard<std::mutex> l(cb_mtx); on_message = std::move(f); }
    void set_on_close(std::function<void()> f) { std::lock_guard<std::mutex> l(cb_mtx); on_close = std::move(f); }
    std::function<void(const std::string&, bool)> get_on_message() { std::lock_guard<std::mutex> l(cb_mtx); return on_message; }
    std::function<void()> get_on_close() { std::lock_guard<std::mutex> l(cb_mtx); return on_close; }

    // Fragment reassembly state (RFC 6455 §5.4) — accessed only from the single-stream ws read
    // callback (one in-flight recv per fd), no lock needed.
    std::string frag_buf;
    uint8_t     frag_opcode = 0;
    bool        frag_active = false;

    // Inbox: this connection's callbacks (message, close) run IN ORDER and ONE AT A TIME.
    // Previously every message went to the pool as a separate job: messages of the same connection were processed
    // on different threads at the same time and in reverse order (~10% reversed in 2000 messages).
    std::mutex                         inbox_mtx;
    struct InboxItem { std::function<void()> fn; size_t bytes; };
    std::list<InboxItem>               inbox;   // list: allocates no memory when empty (a deque allocates ~600 B)
    bool                               inbox_running = false;   // a drainer job is on the pool
    // Backpressure: when the inbox exceeds the upper limit, reading from this socket stops; it resumes when it falls below the lower limit
    // (it used to be unbounded: a flood of 1-byte frames grew memory by GBs on a single connection).
    size_t                             inbox_bytes = 0;
    bool                               read_paused = false;

    explicit WsConnection(int fd_, std::string ip = "")
        : fd(fd_), client_ip(std::move(ip)) {}

    // Thread-safe: encode + send text frame (the caller checks it is UTF-8: ws_utf8_valid)
    bool send_text(const std::string& msg);
    // Thread-safe: encode + send binary frame (opcode 0x2) — ws::send_binary
    bool send_binary(const std::string& bytes);

    // Send close frame and mark as closed
    void close_conn();
    // Server is shutting down (end of graceful shutdown): 1001 Going Away frame, without waiting (the process is about to exit).
    void going_away();

    // Raw send (for pong etc.) — caller must hold write_mutex. Dropped if closed.
    bool send_raw(const std::string& frame);

    // Raw socket write — NO closed guard. close_conn must be able to write its own close frame
    // even when closed=true (send_raw would drop it). The lock is the caller's.
    bool write_all(const std::string& frame);

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
    void   broadcast_binary(const std::string& bytes);   // ws::broadcast_binary
    void   going_away_all();                             // shutdown: 1001 to every connection
    size_t count();
};

extern WsRegistry g_ws_registry;

// ── RFC 6455 frame codec ──────────────────────────────────────────────────────

std::string ws_encode_text_frame (const std::string& payload);
std::string ws_encode_binary_frame(const std::string& payload);
// Strict RFC 3629 UTF-8 (overlong encodings, surrogates U+D800–DFFF and anything above U+10FFFF are REJECTED). A text frame
// must be valid UTF-8 (RFC 6455 §8.1): a browser rejects an invalid one by closing the connection.
bool ws_utf8_valid(const std::string& s);
std::string ws_encode_close_frame();
// Close with a status code (RFC 6455 §7.4): 1007 = invalid data (invalid UTF-8 in a text frame).
std::string ws_encode_close_frame_code(uint16_t code);
std::string ws_encode_pong_frame (const std::string& payload);

struct WsFrame {
    bool        fin      = true;
    uint8_t     opcode   = 0;     // 0x1=text, 0x2=bin, 0x8=close, 0x9=ping, 0xA=pong
    std::string payload;
    size_t      consumed = 0;
    bool        complete = false;
    bool        protocol_error = false;  // violation of RFC 6455 §5.1 (e.g. unmasked client frame)
};

// Try to decode one frame from buf — returns complete=false if more data needed.
WsFrame ws_try_decode_frame(const std::string& buf);
WsFrame ws_try_decode_frame(std::string_view buf);

// ── HTTP → WebSocket handshake (RFC 6455 §4) ─────────────────────────────────

std::string ws_accept_key(const std::string& client_key);
std::string ws_handshake_101(const std::string& client_key);

} // namespace look

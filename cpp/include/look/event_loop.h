#pragma once

#include <functional>
#include <string>
#include <memory>
#include <atomic>

namespace look {

// ── Callback types ────────────────────────────────────────────────────────────

using AcceptCb = std::function<void(int fd)>;
using ReadCb   = std::function<void(const char* data, size_t len)>;
using WriteCb  = std::function<void(bool ok)>;
using TaskFn   = std::function<void()>;

// ── Abstract event loop ───────────────────────────────────────────────────────
//
// EventLoop manages non-blocking I/O and a task queue.
// Phase 13: HTTP connection lifecycle (accept → read → write → close).
// Phase 14: parallel() tasks posted via post().
// Phase 15: WebSocket frame I/O on top of the same loop.
//
// --mode fcgi does NOT use EventLoop — ThreadPool handles that path.
// --mode http creates one EventLoop per process; workers post tasks to it.

class EventLoop {
public:
    virtual ~EventLoop() = default;

    // Start the loop — blocks until stop() is called.
    virtual void run() = 0;

    // Signal loop to stop after current iteration.
    virtual void stop() = 0;

    // Post a task to the loop's task queue (thread-safe).
    // Used by worker threads to schedule callbacks back on the loop thread.
    virtual void post(TaskFn fn) = 0;

    // Register server socket — calls cb with each accepted client fd.
    virtual void listen(int server_fd, AcceptCb cb) = 0;

    // Register client fd for reading — calls cb when data arrives. Also resumes a paused read.
    virtual void async_read(int fd, ReadCb cb) = 0;

    // Back pressure: stop reading from this fd (only on the loop thread, also from inside the
    // read callback). The read callback is persistent; the only way to pause it is this — async_read resumes it. An edge-
    // triggered read goes on until EAGAIN: a client that sends data without pause would never be stopped.
    // Default: none (Windows IOCP — development environment).
    virtual void pause_read(int /*fd*/) {}

    // Add a new (blocking) fd to the loop: set_nonblocking + EPOLL_CTL_ADD + read registration.
    // WS/SSE upgrade: to hand a blocking fd on a worker thread over to the async loop.
    virtual void add_client(int fd, ReadCb cb) = 0;

    // Write data to fd — calls cb when done (or on error, ok=false).
    virtual void async_write(int fd, std::string data, WriteCb cb) = 0;

    // Remove fd from the loop and close it.
    virtual void close_fd(int fd) = 0;

    // Idle/slowloris protection: if this fd sends no data for `seconds` the loop
    // closes it itself (close_fd → len==0 reaches the upper layer's read cb).
    // OPT-IN: only fds that make this call are scanned; legitimately long-idle
    // connections such as HTTP/WS/SSE are not affected. seconds<=0 → disabled (default).
    // Because SO_RCVTIMEO does not work with epoll ET + non-blocking sockets the timeout
    // logic lives in this layer (on the loop thread, the same thread as the I/O cb → no race).
    virtual void set_idle_timeout(int fd, int seconds) = 0;

    // Remove fd from the loop WITHOUT closing the socket.
    // Used by STARTTLS: EventLoop relinquishes the fd to a worker thread for
    // SSL_accept(), then the caller re-registers via add_client() afterwards.
    virtual void detach_fd(int fd) = 0;

    // Factory — returns platform-appropriate implementation.
    static std::unique_ptr<EventLoop> create();
};

// ── Platform implementations ──────────────────────────────────────────────────

#if defined(__linux__)

// Linux: epoll-based, edge-triggered, minimal external runtime dependencies.
class EpollEventLoop : public EventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

    void run() override;
    void stop() override;
    void post(TaskFn fn) override;
    void listen(int server_fd, AcceptCb cb) override;
    void async_read(int fd, ReadCb cb) override;
    void pause_read(int fd) override;
    void add_client(int fd, ReadCb cb) override;
    void async_write(int fd, std::string data, WriteCb cb) override;
    void close_fd(int fd) override;
    void set_idle_timeout(int fd, int seconds) override;
    void detach_fd(int fd) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#elif defined(_WIN32)

// Windows: IOCP-based, unlimited connections, production-grade.
class IocpEventLoop : public EventLoop {
public:
    IocpEventLoop();
    ~IocpEventLoop() override;

    void run() override;
    void stop() override;
    void post(TaskFn fn) override;
    void listen(int server_fd, AcceptCb cb) override;
    void async_read(int fd, ReadCb cb) override;
    void pause_read(int fd) override;   // back pressure: a completed read is not re-armed (async_read resumes it)
    void add_client(int fd, ReadCb cb) override;
    void async_write(int fd, std::string data, WriteCb cb) override;
    void close_fd(int fd) override;
    void set_idle_timeout(int fd, int seconds) override;
    void detach_fd(int fd) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

// ── Thread-local EventLoop accessor ─────────────────────────────────────────
// Set by each worker thread at startup so that DB fiber async I/O
// can register callbacks without requiring a global event loop pointer.
EventLoop* get_thread_event_loop() noexcept;
void        set_thread_event_loop(EventLoop* loop) noexcept;

} // namespace look

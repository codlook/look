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

    // Register client fd for reading — calls cb when data arrives.
    virtual void async_read(int fd, ReadCb cb) = 0;

    // Yeni (blocking) fd'yi loop'a ekle: set_nonblocking + EPOLL_CTL_ADD + read kaydı.
    // WS/SSE upgrade: worker thread'deki blocking fd'yi async loop'a devretmek için.
    virtual void add_client(int fd, ReadCb cb) = 0;

    // Write data to fd — calls cb when done (or on error, ok=false).
    virtual void async_write(int fd, std::string data, WriteCb cb) = 0;

    // Remove fd from the loop and close it.
    virtual void close_fd(int fd) = 0;

    // Factory — returns platform-appropriate implementation.
    static std::unique_ptr<EventLoop> create();
};

// ── Platform implementations ──────────────────────────────────────────────────

#if defined(__linux__)

// Linux: epoll-based, edge-triggered, sıfır bağımlılık.
class EpollEventLoop : public EventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

    void run() override;
    void stop() override;
    void post(TaskFn fn) override;
    void listen(int server_fd, AcceptCb cb) override;
    void async_read(int fd, ReadCb cb) override;
    void add_client(int fd, ReadCb cb) override;
    void async_write(int fd, std::string data, WriteCb cb) override;
    void close_fd(int fd) override;

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
    void add_client(int fd, ReadCb cb) override;
    void async_write(int fd, std::string data, WriteCb cb) override;
    void close_fd(int fd) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace look

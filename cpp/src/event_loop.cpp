#include "look/event_loop.h"

#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <vector>
#include <cstring>
#include <chrono>

// ── Platform headers ──────────────────────────────────────────────────────────

#if defined(__linux__)
#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <errno.h>
#elif defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mswsock.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "Mswsock.lib")
#endif

namespace look {

// =============================================================================
// Linux -- EpollEventLoop
// =============================================================================

#if defined(__linux__)

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct FdEntry {
    enum class Kind { SERVER, CLIENT } kind = Kind::CLIENT;
    AcceptCb    accept_cb;
    ReadCb      read_cb;
    WriteCb     write_cb;
    std::string write_buf;
    size_t      write_pos = 0;
    std::mutex  mtx;
    std::atomic<bool> closed{false};   // after close_fd, prevents the read loop from reading again / a double close
    std::atomic<bool> read_paused{false};   // backpressure: the read loop does not read (pause_read / async_read)

    // Idle/slowloris tracking (opt-in). If idle_timeout_sec>0 the loop scans this fd.
    // last_activity is written only on the loop thread (accept + every read) → no lock.
    int idle_timeout_sec = 0;
    std::chrono::steady_clock::time_point last_activity{};
};

struct EpollEventLoop::Impl {
    int epfd      = -1;
    int wakeup_fd = -1;
    std::atomic<bool> running{false};

    std::unordered_map<int, std::shared_ptr<FdEntry>> fds;
    std::mutex map_mtx;

    std::queue<TaskFn> tasks;
    std::mutex tasks_mtx;

    // How many fds are tracked for idle-timeout. If 0, epoll_wait waits forever (no extra
    // wakeup in an HTTP-only deployment — no regression); if >0, a finite tick + scan.
    std::atomic<int> idle_watch_count{0};

    std::shared_ptr<FdEntry> get(int fd) {
        std::lock_guard<std::mutex> lk(map_mtx);
        auto it = fds.find(fd);
        return it != fds.end() ? it->second : nullptr;
    }
};

EpollEventLoop::EpollEventLoop() : impl_(std::make_unique<Impl>()) {
    impl_->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (impl_->epfd < 0) throw std::runtime_error("epoll_create1 failed");

    impl_->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl_->wakeup_fd < 0) throw std::runtime_error("eventfd failed");

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = impl_->wakeup_fd;
    epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, impl_->wakeup_fd, &ev);
}

EpollEventLoop::~EpollEventLoop() {
    if (impl_->wakeup_fd >= 0) ::close(impl_->wakeup_fd);
    if (impl_->epfd >= 0)      ::close(impl_->epfd);
}

void EpollEventLoop::post(TaskFn fn) {
    {
        std::lock_guard<std::mutex> lk(impl_->tasks_mtx);
        impl_->tasks.push(std::move(fn));
    }
    uint64_t val = 1;
    (void)write(impl_->wakeup_fd, &val, sizeof(val));
}

void EpollEventLoop::stop() {
    impl_->running = false;
    post([]{});
}

void EpollEventLoop::listen(int server_fd, AcceptCb cb) {
    set_nonblocking(server_fd);

    auto entry       = std::make_shared<FdEntry>();
    entry->kind      = FdEntry::Kind::SERVER;
    entry->accept_cb = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(impl_->map_mtx);
        impl_->fds[server_fd] = entry;
    }

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, server_fd, &ev);
}

void EpollEventLoop::add_client(int fd, ReadCb cb) {
    set_nonblocking(fd);
    auto entry     = std::make_shared<FdEntry>();
    entry->kind    = FdEntry::Kind::CLIENT;
    entry->read_cb = std::move(cb);
    entry->last_activity = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(impl_->map_mtx);
        impl_->fds[fd] = entry;
    }
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev);
}

void EpollEventLoop::pause_read(int fd) {
    if (auto entry = impl_->get(fd)) entry->read_paused.store(true);
}

void EpollEventLoop::async_read(int fd, ReadCb cb) {
    auto entry = impl_->get(fd);
    if (!entry) return;
    {
        std::lock_guard<std::mutex> lk(entry->mtx);
        entry->read_cb = std::move(cb);
    }
    // Resume if paused. EPOLL_CTL_MOD re-evaluates readiness: for data that arrived
    // during the pause the event fires again (not lost with edge triggering).
    entry->read_paused.store(false);
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev);
}

void EpollEventLoop::async_write(int fd, std::string data, WriteCb cb) {
    auto entry = impl_->get(fd);
    if (!entry) { if (cb) cb(false); return; }
    {
        std::lock_guard<std::mutex> lk(entry->mtx);
        entry->write_buf = std::move(data);
        entry->write_pos = 0;
        entry->write_cb  = std::move(cb);
    }
    epoll_event ev{};
    ev.events  = EPOLLOUT;
    ev.data.fd = fd;
    epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev);
}

void EpollEventLoop::close_fd(int fd) {
    // Idempotent: if the fd was already removed from the map (already closed), return early — avoids a double
    // ::close(fd). In a multi-threaded server, if an intervening accept reused the same fd
    // number, the second ::close would close the WRONG connection.
    std::shared_ptr<FdEntry> entry;
    {
        std::lock_guard<std::mutex> lk(impl_->map_mtx);
        auto it = impl_->fds.find(fd);
        if (it == impl_->fds.end()) return;
        entry = it->second;
        impl_->fds.erase(it);
    }
    entry->closed.store(true);
    if (entry->idle_timeout_sec > 0) impl_->idle_watch_count.fetch_sub(1);
    epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
}

void EpollEventLoop::set_idle_timeout(int fd, int seconds) {
    auto entry = impl_->get(fd);
    if (!entry) return;
    int old = entry->idle_timeout_sec;
    entry->idle_timeout_sec = seconds;
    entry->last_activity    = std::chrono::steady_clock::now();
    if (seconds > 0 && old <= 0)      impl_->idle_watch_count.fetch_add(1);
    else if (seconds <= 0 && old > 0) impl_->idle_watch_count.fetch_sub(1);
}

void EpollEventLoop::detach_fd(int fd) {
    // Remove from epoll and state map WITHOUT closing the socket.
    // Used by STARTTLS: worker thread takes over the fd for SSL_accept(),
    // then calls add_client() to return it to the loop.
    epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr);
    std::lock_guard<std::mutex> lk(impl_->map_mtx);
    auto it = impl_->fds.find(fd);
    if (it != impl_->fds.end()) {
        it->second->closed.store(true);
        if (it->second->idle_timeout_sec > 0) impl_->idle_watch_count.fetch_sub(1);
        impl_->fds.erase(it);
    }
}

void EpollEventLoop::run() {
    impl_->running = true;
    constexpr int MAX_EVENTS = 512;
    epoll_event events[MAX_EVENTS];
    char read_buf[65536];

    while (impl_->running) {
        // If there are fds tracked for idle-timeout, a finite tick (1 s) — scan on every wakeup.
        // If there are none, -1 (infinite): no extra wakeup in an HTTP-only deployment.
        int wait_ms = impl_->idle_watch_count.load() > 0 ? 1000 : -1;
        int n = epoll_wait(impl_->epfd, events, MAX_EVENTS, wait_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // ── Idle scan (loop thread — same thread as the I/O cb → no race) ──
        // Runs on a timeout wakeup (n==0) or a normal wakeup. Among CLIENT fds with idle_timeout_sec>0,
        // close those whose time since the last activity has expired.
        if (impl_->idle_watch_count.load() > 0) {
            auto now = std::chrono::steady_clock::now();
            std::vector<int> expired;
            {
                std::lock_guard<std::mutex> lk(impl_->map_mtx);
                for (auto& kv : impl_->fds) {
                    auto& e = kv.second;
                    if (e->idle_timeout_sec <= 0 || e->closed.load()) continue;
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                                    now - e->last_activity).count();
                    if (idle >= e->idle_timeout_sec) expired.push_back(kv.first);
                }
            }
            for (int efd : expired) {
                auto e = impl_->get(efd);
                if (!e) continue;
                ReadCb cb;
                { std::lock_guard<std::mutex> lk(e->mtx); cb = e->read_cb; }
                if (cb) cb(read_buf, 0);   // notify the upper layer of the disconnect (cleanup)
                close_fd(efd);             // fd+conn_count cleanup is in the upper layer
            }
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == impl_->wakeup_fd) {
                uint64_t val;
                (void)read(impl_->wakeup_fd, &val, sizeof(val));
                std::queue<TaskFn> local;
                {
                    std::lock_guard<std::mutex> lk(impl_->tasks_mtx);
                    std::swap(local, impl_->tasks);
                }
                while (!local.empty()) { local.front()(); local.pop(); }
                continue;
            }

            auto entry = impl_->get(fd);
            if (!entry) continue;

            if (entry->kind == FdEntry::Kind::SERVER) {
                AcceptCb acb;
                { std::lock_guard<std::mutex> lk(entry->mtx); acb = entry->accept_cb; }
                while (true) {
                    int client = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client < 0) break;
                    auto ce   = std::make_shared<FdEntry>();
                    ce->kind  = FdEntry::Kind::CLIENT;
                    ce->last_activity = std::chrono::steady_clock::now();
                    {
                        std::lock_guard<std::mutex> lk(impl_->map_mtx);
                        impl_->fds[client] = ce;
                    }
                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLET;
                    cev.data.fd = client;
                    epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, client, &cev);
                    if (acb) acb(client);
                }
                continue;
            }

            if (events[i].events & EPOLLIN) {
                ReadCb cb;
                { std::lock_guard<std::mutex> lk(entry->mtx); cb = entry->read_cb; }
                while (true) {
                    if (entry->read_paused.load()) break;   // backpressure: async_read resumes
                    ssize_t r = read(fd, read_buf, sizeof(read_buf));
                    if (r > 0) {
                        if (entry->idle_timeout_sec > 0)
                            entry->last_activity = std::chrono::steady_clock::now();
                        if (cb) cb(read_buf, (size_t)r);
                        // cb may have closed its own fd (e.g. WS close frame).
                        // If closed, break out of the loop: avoid reading again from a closed/reopened fd
                        // and delivering data to another connection.
                        if (entry->closed.load()) break;
                    } else if (r == 0) {
                        // Clean FIN — notify the callback with len==0 so the upper layer
                        // (WS/SSE) does its registry+buffer cleanup (otherwise every
                        // disconnect leaks → cap-hit DoS + stale-fd).
                        if (cb) cb(read_buf, 0);
                        close_fd(fd); break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (cb) cb(read_buf, 0);   // an error is also a disconnect — report cleanup
                        close_fd(fd); break;
                    }
                }
            }

            if ((events[i].events & EPOLLOUT) && !entry->closed.load()) {
                WriteCb cb;
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(entry->mtx);
                    while (entry->write_pos < entry->write_buf.size()) {
                        ssize_t w = write(fd,
                            entry->write_buf.data() + entry->write_pos,
                            entry->write_buf.size() - entry->write_pos);
                        if (w > 0) {
                            entry->write_pos += (size_t)w;
                        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break;
                        } else {
                            entry->write_pos = entry->write_buf.size();
                            break;
                        }
                    }
                    done = (entry->write_pos >= entry->write_buf.size());
                    if (done) {
                        cb = entry->write_cb;
                        entry->write_cb = nullptr;
                        entry->write_buf.clear();
                        entry->write_pos = 0;
                    }
                }
                if (done) {
                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLET;
                    cev.data.fd = fd;
                    epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &cev);
                    if (cb) cb(true);
                }
            }
        }
    }
}

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<EpollEventLoop>();
}

// =============================================================================
// Windows -- IocpEventLoop
// IOCP: AcceptEx + WSARecv + WSASend, unlimited connections
// =============================================================================

#elif defined(_WIN32)

// Per-operation context -- OVERLAPPED must be first member
enum class IocpOp { ACCEPT, READ, WRITE };

struct IocpOv {
    OVERLAPPED  ov;                 // must be first
    IocpOp      op;
    SOCKET      sock;
    WSABUF      wsabuf;
    char        buf[65536];
    SOCKET      accept_sock = INVALID_SOCKET;
    char        accept_buf[512];    // (sizeof(sockaddr_in)+16)*2
    std::string write_data;
    WriteCb     write_cb;

    IocpOv() { ZeroMemory(&ov, sizeof(ov)); }
};

static constexpr ULONG_PTR TASK_KEY = static_cast<ULONG_PTR>(-1);
static constexpr ULONG_PTR STOP_KEY = static_cast<ULONG_PTR>(-2);

struct SockState {
    SOCKET   sock      = INVALID_SOCKET;
    bool     is_server = false;
    bool     closed    = false;
    AcceptCb accept_cb;
    ReadCb   read_cb;
    std::mutex mtx;
    // Backpressure (same meaning as Linux EpollEventLoop::pause_read): when paused, a completed read
    // is not re-armed; async_read resumes it. read_pending: a WSARecv is in flight (under mtx).
    bool     read_paused  = false;
    bool     read_pending = false;

    // Idle/slowloris tracking (opt-in) — parallel to Linux EpollEventLoop.
    int idle_timeout_sec = 0;
    std::chrono::steady_clock::time_point last_activity{};
};

struct IocpEventLoop::Impl {
    HANDLE iocp = INVALID_HANDLE_VALUE;
    std::atomic<bool> running{false};

    std::unordered_map<SOCKET, std::shared_ptr<SockState>> states;
    std::mutex states_mtx;

    std::queue<TaskFn> tasks;
    std::mutex tasks_mtx;

    // Number of sockets tracked for idle-timeout — if 0 GQCS is INFINITE, if >0 a finite tick.
    std::atomic<int> idle_watch_count{0};

    LPFN_ACCEPTEX fn_acceptex = nullptr;

    std::shared_ptr<SockState> get_state(SOCKET s) {
        std::lock_guard<std::mutex> lk(states_mtx);
        auto it = states.find(s);
        return it != states.end() ? it->second : nullptr;
    }

    bool post_read(std::shared_ptr<SockState> st) {
        if (!st || st->closed) return false;
        auto* ov    = new IocpOv();
        ov->op      = IocpOp::READ;
        ov->sock    = st->sock;
        ov->wsabuf  = { sizeof(ov->buf), ov->buf };
        DWORD flags = 0, bytes = 0;
        { std::lock_guard<std::mutex> lk(st->mtx); st->read_pending = true; }
        int r = WSARecv(st->sock, &ov->wsabuf, 1, &bytes, &flags, &ov->ov, nullptr);
        if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            { std::lock_guard<std::mutex> lk(st->mtx); st->read_pending = false; }
            delete ov;
            return false;
        }
        return true;
    }

    bool post_accept(std::shared_ptr<SockState> server_st) {
        if (!fn_acceptex || !server_st || server_st->closed) return false;
        SOCKET as = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                              nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (as == INVALID_SOCKET) return false;

        auto* ov        = new IocpOv();
        ov->op          = IocpOp::ACCEPT;
        ov->sock        = server_st->sock;
        ov->accept_sock = as;

        DWORD recvd = 0;
        BOOL ok = fn_acceptex(server_st->sock, as,
                              ov->accept_buf, 0,
                              sizeof(sockaddr_in) + 16,
                              sizeof(sockaddr_in) + 16,
                              &recvd, &ov->ov);
        if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
            closesocket(as);
            delete ov;
            return false;
        }
        return true;
    }

    void close_internal(SOCKET s) {
        std::shared_ptr<SockState> st;
        {
            std::lock_guard<std::mutex> lk(states_mtx);
            auto it = states.find(s);
            if (it == states.end()) return;
            st = it->second;
            states.erase(it);
        }
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->closed = true;
            if (st->idle_timeout_sec > 0) idle_watch_count.fetch_sub(1);
        }
        CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
        closesocket(s);
    }
};

IocpEventLoop::IocpEventLoop() : impl_(std::make_unique<Impl>()) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    impl_->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (impl_->iocp == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CreateIoCompletionPort failed");

    SOCKET dummy = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    GUID guid    = WSAID_ACCEPTEX;
    DWORD bytes  = 0;
    WSAIoctl(dummy, SIO_GET_EXTENSION_FUNCTION_POINTER,
             &guid, sizeof(guid),
             &impl_->fn_acceptex, sizeof(impl_->fn_acceptex),
             &bytes, nullptr, nullptr);
    closesocket(dummy);
}

IocpEventLoop::~IocpEventLoop() {
    if (impl_->iocp != INVALID_HANDLE_VALUE) CloseHandle(impl_->iocp);
    WSACleanup();
}

void IocpEventLoop::post(TaskFn fn) {
    {
        std::lock_guard<std::mutex> lk(impl_->tasks_mtx);
        impl_->tasks.push(std::move(fn));
    }
    PostQueuedCompletionStatus(impl_->iocp, 0, TASK_KEY, nullptr);
}

void IocpEventLoop::stop() {
    impl_->running = false;
    PostQueuedCompletionStatus(impl_->iocp, 0, STOP_KEY, nullptr);
}

void IocpEventLoop::listen(int server_fd, AcceptCb cb) {
    SOCKET s = static_cast<SOCKET>(server_fd);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), impl_->iocp,
                           static_cast<ULONG_PTR>(s), 0);

    auto st = std::make_shared<SockState>();
    st->sock      = s;
    st->is_server = true;
    st->accept_cb = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(impl_->states_mtx);
        impl_->states[s] = st;
    }

    // Pipeline: 4 pending AcceptEx for burst accept performance
    impl_->post_accept(st);
    impl_->post_accept(st);
    impl_->post_accept(st);
    impl_->post_accept(st);
}

void IocpEventLoop::add_client(int fd, ReadCb cb) {
    SOCKET s = static_cast<SOCKET>(fd);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), impl_->iocp,
                           static_cast<ULONG_PTR>(s), 0);

    auto st = std::make_shared<SockState>();
    st->sock    = s;
    st->read_cb = std::move(cb);
    st->last_activity = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(impl_->states_mtx);
        impl_->states[s] = st;
    }
    impl_->post_read(st);
}

void IocpEventLoop::async_read(int fd, ReadCb cb) {
    auto st = impl_->get_state(static_cast<SOCKET>(fd));
    if (!st) return;
    bool repost = false;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->read_cb = std::move(cb);
        // If a WSARecv is in flight, its completion uses the new callback; if paused (backpressure)
        // the read is re-armed here.
        if (st->read_paused) { st->read_paused = false; repost = !st->read_pending && !st->closed; }
    }
    if (repost) impl_->post_read(st);
}

void IocpEventLoop::pause_read(int fd) {
    auto st = impl_->get_state(static_cast<SOCKET>(fd));
    if (!st) return;
    std::lock_guard<std::mutex> lk(st->mtx);
    st->read_paused = true;   // the in-flight read completes, then is not re-armed
}

void IocpEventLoop::async_write(int fd, std::string data, WriteCb cb) {
    SOCKET s = static_cast<SOCKET>(fd);
    auto   st = impl_->get_state(s);
    if (!st || st->closed) { if (cb) cb(false); return; }

    auto* ov       = new IocpOv();
    ov->op         = IocpOp::WRITE;
    ov->sock       = s;
    ov->write_data = std::move(data);
    ov->write_cb   = std::move(cb);
    ov->wsabuf     = { static_cast<ULONG>(ov->write_data.size()),
                       const_cast<char*>(ov->write_data.data()) };

    DWORD sent = 0;
    int r = WSASend(s, &ov->wsabuf, 1, &sent, 0, &ov->ov, nullptr);
    if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        if (ov->write_cb) ov->write_cb(false);
        delete ov;
    }
}

void IocpEventLoop::close_fd(int fd) {
    impl_->close_internal(static_cast<SOCKET>(fd));
}

void IocpEventLoop::detach_fd(int fd) {
    // Cancel pending I/O and remove from state map WITHOUT closing the socket.
    SOCKET s = static_cast<SOCKET>(fd);
    std::shared_ptr<SockState> st;
    {
        std::lock_guard<std::mutex> lk(impl_->states_mtx);
        auto it = impl_->states.find(s);
        if (it == impl_->states.end()) return;
        st = it->second;
        impl_->states.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->closed = true;
        if (st->idle_timeout_sec > 0) impl_->idle_watch_count.fetch_sub(1);
    }
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
    // Do NOT closesocket(s) — caller owns the socket now
}

void IocpEventLoop::set_idle_timeout(int fd, int seconds) {
    auto st = impl_->get_state(static_cast<SOCKET>(fd));
    if (!st) return;
    std::lock_guard<std::mutex> lk(st->mtx);
    int old = st->idle_timeout_sec;
    st->idle_timeout_sec = seconds;
    st->last_activity    = std::chrono::steady_clock::now();
    if (seconds > 0 && old <= 0)      impl_->idle_watch_count.fetch_add(1);
    else if (seconds <= 0 && old > 0) impl_->idle_watch_count.fetch_sub(1);
}

void IocpEventLoop::run() {
    impl_->running = true;

    while (impl_->running) {
        DWORD      bytes = 0;
        ULONG_PTR  key   = 0;
        OVERLAPPED* pov  = nullptr;

        // If there are sockets tracked for idle-timeout, a finite tick (1 s), otherwise INFINITE.
        DWORD wait_ms = impl_->idle_watch_count.load() > 0 ? 1000 : INFINITE;
        BOOL ok = GetQueuedCompletionStatus(impl_->iocp, &bytes, &key, &pov, wait_ms);

        // ── Idle scan (loop thread — same thread as the I/O cb → no race) ──
        // Runs after a GQCS timeout (ok==FALSE, pov==NULL, WAIT_TIMEOUT) or a normal
        // wakeup. Close sockets whose time has expired.
        if (impl_->idle_watch_count.load() > 0) {
            auto now = std::chrono::steady_clock::now();
            std::vector<SOCKET> expired;
            {
                std::lock_guard<std::mutex> lk(impl_->states_mtx);
                for (auto& kv : impl_->states) {
                    auto& st = kv.second;
                    std::lock_guard<std::mutex> slk(st->mtx);
                    if (st->idle_timeout_sec <= 0 || st->closed) continue;
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                                    now - st->last_activity).count();
                    if (idle >= st->idle_timeout_sec) expired.push_back(kv.first);
                }
            }
            for (SOCKET es : expired) {
                auto st = impl_->get_state(es);
                if (!st) continue;
                ReadCb cb;
                { std::lock_guard<std::mutex> slk(st->mtx); cb = st->read_cb; }
                if (cb) cb(nullptr, 0);      // notify the upper layer of the disconnect
                impl_->close_internal(es);
            }
        }

        if (!ok && pov == nullptr) {
            // Timeout (WAIT_TIMEOUT) or an orphaned completion — go back to the loop.
            if (GetLastError() == WAIT_TIMEOUT) continue;
        }

        if (key == STOP_KEY) break;

        if (key == TASK_KEY) {
            std::queue<TaskFn> local;
            {
                std::lock_guard<std::mutex> lk(impl_->tasks_mtx);
                std::swap(local, impl_->tasks);
            }
            while (!local.empty()) { local.front()(); local.pop(); }
            continue;
        }

        if (!pov) continue;

        auto* ov = reinterpret_cast<IocpOv*>(pov);

        if (ov->op == IocpOp::ACCEPT) {
            SOCKET server_s = ov->sock;
            SOCKET client_s = ov->accept_sock;
            ov->accept_sock = INVALID_SOCKET;

            auto server_st = impl_->get_state(server_s);

            if (!ok || !server_st || server_st->closed) {
                if (client_s != INVALID_SOCKET) closesocket(client_s);
                delete ov;
                if (server_st && !server_st->closed) impl_->post_accept(server_st);
                continue;
            }

            // Required by AcceptEx: inherit listening socket options
            setsockopt(client_s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<char*>(&server_s), sizeof(server_s));

            CreateIoCompletionPort(reinterpret_cast<HANDLE>(client_s), impl_->iocp,
                                   static_cast<ULONG_PTR>(client_s), 0);

            auto client_st  = std::make_shared<SockState>();
            client_st->sock = client_s;
            client_st->last_activity = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lk(impl_->states_mtx);
                impl_->states[client_s] = client_st;
            }

            AcceptCb acb;
            { std::lock_guard<std::mutex> lk(server_st->mtx); acb = server_st->accept_cb; }
            if (acb) acb(static_cast<int>(client_s));

            // Start reading (accept_cb may have already set read_cb via async_read)
            impl_->post_read(client_st);

            delete ov;

            // Restock accept pipeline
            impl_->post_accept(server_st);
            continue;
        }

        if (ov->op == IocpOp::READ) {
            SOCKET s  = ov->sock;
            auto   st = impl_->get_state(s);

            if (!ok || bytes == 0) {
                delete ov;
                if (st && !st->closed) impl_->close_internal(s);
                continue;
            }

            if (st && !st->closed) {
                ReadCb cb;
                {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    st->read_pending = false;
                    cb = st->read_cb;
                    if (st->idle_timeout_sec > 0)
                        st->last_activity = std::chrono::steady_clock::now();
                }
                if (cb) cb(ov->buf, static_cast<size_t>(bytes));
                bool repost;
                { std::lock_guard<std::mutex> lk(st->mtx); repost = !st->closed && !st->read_paused && !st->read_pending; }
                if (repost) impl_->post_read(st);
            }

            delete ov;
            continue;
        }

        if (ov->op == IocpOp::WRITE) {
            WriteCb cb   = std::move(ov->write_cb);
            bool    good = ok && (bytes > 0);
            delete ov;
            if (cb) cb(good);
            continue;
        }

        delete ov;
    }
}

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<IocpEventLoop>();
}

#endif

// ── Thread-local EventLoop accessor ─────────────────────────────────────────

// fiber-ok: the thread's own event loop (thread structure)
static thread_local EventLoop* tl_event_loop = nullptr;

EventLoop* get_thread_event_loop() noexcept { return tl_event_loop; }
void        set_thread_event_loop(EventLoop* loop) noexcept { tl_event_loop = loop; }

} // namespace look

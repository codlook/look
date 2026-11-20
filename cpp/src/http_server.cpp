#include "look/http_server.h"
#include "look/http_parse.h"   // pure parse_request seam (fuzz + test target)
#include "look/websocket.h"
#include "look/sse.h"
#include "look/event_loop.h"
#include "look/fiber.h"
#include "look/logger.h"
#include "look/thread_start.h"   // start_pool_threads: if it cannot start, stop + join + error with a reason

#include <sstream>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <queue>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#ifdef __linux__
#include <sys/resource.h>
#include <poll.h>   // watcher: does the stuck worker's socket have a waiting connection
#endif

// LOOK imposes no limit on the number of open connections; the limit is the operating system's descriptor limit.
// The soft limit is 1024 on most systems: it is raised to the hard limit at startup (Go does the same).
static void raise_fd_limit() {
#ifdef __linux__
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif
}

// Dispatch model (1.0.20): on Linux fiber is the DEFAULT — it does not hold a worker while waiting on a connection.
// "0" = the old model (one worker per connection). There is no fiber dispatch outside Linux.
static bool fiber_dispatch_on() {
#ifdef __linux__
    const char* v = std::getenv("LOOK_FIBER_DISPATCH");
    return !(v && std::string(v) == "0");
#else
    return false;
#endif
}

// If accept fails because descriptors/memory are exhausted, the connection waits in the queue and accept immediately
// returns the same error again: returning without waiting burns the kernel at 100%. Wait briefly, retry;
// when one of the open connections closes, accepting resumes by itself.
static void accept_backoff() {
#ifndef _WIN32
    int e = errno;
    if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
}

#if defined(__linux__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define close(x)   closesocket(x)
#  define ssize_t    int
#  define recv(f,b,l,fl) recv((SOCKET)(f),(b),(int)(l),(fl))
#  define send(f,b,l,fl) send((SOCKET)(f),(b),(int)(l),(fl))
#endif

#if defined(__linux__)
#  include <poll.h>
#endif

namespace look {

// fcgi_main picks the default worker count according to the dispatch model (see fiber_dispatch_on).
bool http_fiber_dispatch_enabled() { return fiber_dispatch_on(); }

// ── HTTP/1.1 parser ───────────────────────────────────────────────────────────
// Pure parsing logic moved to look/http_parse.h (fuzz + test target, no
// drift). The thin wrapper here preserves the existing call sites.

static int parse_request(const std::string& raw, HttpRequest& req) {
    return http_parse_request(raw, req);
}

// ── HTTP response builder ─────────────────────────────────────────────────────

std::string HttpResponse::build() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    // Defense-in-depth: CR/LF strip — prevents HTTP response splitting
    // (the input layer response::header/redirect/cookie already sanitizes).
    auto strip_crlf = [](const std::string& s) {
        size_t cut = s.find_first_of("\r\n");
        return cut == std::string::npos ? s : s.substr(0, cut);
    };
    for (auto& [k, v] : headers) out << strip_crlf(k) << ": " << strip_crlf(v) << "\r\n";
    // Each cookie is a separate Set-Cookie line (a std::map cannot hold the same key more than once)
    for (auto& c : set_cookies) out << "Set-Cookie: " << strip_crlf(c) << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    out << "\r\n";
    out << body;
    return out.str();
}

// ── Worker thread pool ────────────────────────────────────────────────────────

class WorkerPool {
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
public:
    explicit WorkerPool(int n) {
        look::start_pool_threads(workers_, n, "HTTP worker pool", [this] {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
                        if (stop_ && queue_.empty()) return;
                        fn = std::move(queue_.front());
                        queue_.pop();
                    }
                    fn();
                }
            },
            [this] { { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; } cv_.notify_all(); });
    }
    void submit(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(mtx_); queue_.push(std::move(fn)); }
        cv_.notify_one();
    }
    ~WorkerPool() {
        { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
};

// ── HttpServer::Impl ──────────────────────────────────────────────────────────

static void send_ws_reject(int fd, const std::string& reason) {
    std::string resp =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(reason.size()) + "\r\n"
        "\r\n" + reason;
#ifdef _WIN32
    ::send(fd, resp.data(), (int)resp.size(), 0);
#else
    ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
#endif
}

// ── Per-IP concurrent-connection limit (worker-scope) ─────────────────────────
// The HTTP counterpart of the LOOK_SMTP_MAX_CONNS_IP model on the SMTP/IMAP side. Prevents a single
// IP from exhausting the worker pool (makes slow-body/slowloris much harder from a single source
// — a defense layer together with ①). The counter stays incremented for as long as the connection holds
// a worker (handle_connection RAII scope); when WS/SSE is handed over to the
// event loop the worker is freed and the slot is freed too — this is exactly
// the threshold of "how many workers can one IP hold".
//
// DEFAULT OFF (0). The key is the TCP peer IP; in PROD LOOK sits BEHIND a reverse proxy
// (nginx/Apache) → all connections appear from the proxy's IP, and a single
// counter would throttle the whole site. So it should be enabled only when LOOK faces the internet
// directly (LOOK_HTTP_MAX_CONNS_IP=N); behind a proxy, rate limiting is done at the proxy.
// (① same rationale as LOOK_BODY_MIN_RATE: opt-in that does not break proxied setups.)
static int http_max_conns_per_ip() {
    static int v = []() {
        const char* e = std::getenv("LOOK_HTTP_MAX_CONNS_IP");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 0;   // 0/negative/unset = disabled (opt-in)
    }();
    return v;
}
static std::mutex                            g_http_ip_mutex;
static std::unordered_map<std::string, int>  g_http_ip_conns;

// An address hitting the limit is logged ONCE (not repeated until the counter drops below the limit):
// so the log shows whether the limit is cutting real users or an attack,
// but an attack cannot fill the log either. Called under g_http_ip_mutex.
static std::unordered_map<std::string, bool> g_ip_limit_logged;
static void note_ip_limit(const std::string& ip, int lim, const char* what) {
    if (g_ip_limit_logged[ip]) return;
    g_ip_limit_logged[ip] = true;
    look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
        std::string(what) + " from " + ip + " refused with 429: the address is at its limit of " +
        std::to_string(lim) + " (logged once until it drops below)");
}

// true → slot acquired (always true if the limit is off). false → IP is at the ceiling.
static bool http_ip_acquire(const std::string& ip) {
    int lim = http_max_conns_per_ip();
    if (lim <= 0 || ip.empty()) return true;   // disabled or IP unknown
    std::lock_guard<std::mutex> lk(g_http_ip_mutex);
    int& n = g_http_ip_conns[ip];
    if (n >= lim) { note_ip_limit(ip, lim, "a connection"); return false; }
    ++n; return true;
}
static void http_ip_release(const std::string& ip) {
    if (http_max_conns_per_ip() <= 0 || ip.empty()) return;
    std::lock_guard<std::mutex> lk(g_http_ip_mutex);
    auto it = g_http_ip_conns.find(ip);
    if (it == g_http_ip_conns.end()) return;
    --it->second;
    g_ip_limit_logged.erase(ip);
    if (it->second <= 0) g_http_ip_conns.erase(it);
}

// ── Long-lived connections (WebSocket / SSE): DEFAULT per-address cap ──
// There is no limit on the total number of open connections (the ceiling is the operating system's file limit). So
// a single address, with nothing in front, can consume all of the process's descriptors.
// If LOOK faces the internet directly (LOOK_TRUSTED_PROXY unset), one address holds at most
// HELD_DEFAULT_PER_IP open WebSocket/SSE connections. 1000: many users can sit behind a single address
// (mobile carrier, corporate network) — the limit does not cut them, it cuts the single source opening thousands of connections.
// Behind a reverse proxy all connections appear from the proxy's address: the
// default is not applied there, the limit is set at the proxy. If LOOK_HTTP_MAX_CONNS_IP is given it applies
// (it counts all connections) and this default does not kick in.
static constexpr int HELD_DEFAULT_PER_IP = 1000;
static int held_default_limit() {
    static int v = []() {
        if (http_max_conns_per_ip() > 0) return 0;
        const char* e = std::getenv("LOOK_TRUSTED_PROXY");
        return (e && *e) ? 0 : HELD_DEFAULT_PER_IP;
    }();
    return v;
}
static std::unordered_map<std::string, int> g_held_ip_conns;
static bool held_ip_acquire(const std::string& ip) {
    int lim = held_default_limit();
    if (lim <= 0 || ip.empty()) return true;
    std::lock_guard<std::mutex> lk(g_http_ip_mutex);
    int& n = g_held_ip_conns[ip];
    if (n >= lim) { note_ip_limit(ip, lim, "a WebSocket / SSE connection"); return false; }
    ++n; return true;
}
// When the connection closes: under an explicit limit the slot taken over from the worker is released, under the default the held counter is released.
static void held_ip_release(const std::string& ip) {
    if (ip.empty()) return;
    if (http_max_conns_per_ip() > 0) { http_ip_release(ip); return; }
    if (held_default_limit() <= 0) return;
    std::lock_guard<std::mutex> lk(g_http_ip_mutex);
    auto it = g_held_ip_conns.find(ip);
    if (it == g_held_ip_conns.end()) return;
    --it->second;
    g_ip_limit_logged.erase(ip);
    if (it->second <= 0) g_held_ip_conns.erase(it);
}

struct HttpServer::Impl {
    int  port    = 0;
    int  workers = 0;
    HttpHandler  handler;
    WsHandler    ws_handler;
    SseHandler   sse_handler;
    // CSWSH protection (actual behavior — handle_ws_upgrade): EMPTY = safe default
    // SAME-ORIGIN (if Origin is present it must match Host; a cross-origin browser request = the CSWSH
    // vector is rejected; a backend/wscat client without Origin is allowed). SET (LOOK_WS_ORIGINS) = only the origins
    // in the list (Origin is required). NOTE: NOT "empty = allow all origins" (the old
    // comment was misleading; it described the behavior before same-origin was added).
    std::vector<std::string> allowed_origins;

    int  server_fd = -1;
    std::atomic<bool> running{false};

    // Graceful shutdown: once draining is true no new request is read. Connections WAITING for the next request
    // sit in idle_fds; begin_shutdown closes their read side (recv returns 0,
    // the connection closes) — so a keep-alive connection does not hold a worker for the idle period (30 s).
    // Registration/removal and shutdown are under the same lock: a closed fd cannot be a reused fd of another connection.
    std::atomic<bool> draining{false};
    std::mutex idle_mtx;
    std::unordered_set<int> idle_fds;
    // register the fd as idle-waiting; false if shutdown has begun (the caller closes the connection)
    bool idle_enter(int fd) {
        std::lock_guard<std::mutex> lk(idle_mtx);
        if (draining.load(std::memory_order_acquire)) return false;
        idle_fds.insert(fd);
        return true;
    }
    void idle_leave(int fd) {
        std::lock_guard<std::mutex> lk(idle_mtx);
        idle_fds.erase(fd);
    }

#ifdef __linux__
    // At the watcher's request (see watched): accept the connections waiting on the stuck worker's socket in THIS
    // thread (at most 64) and exit — not a permanent second listener (a shared listen that wakes all
    // workers on every connection cut the rate to a quarter with new connections).
    // If the socket is still registered (worker has not exited, shutdown has not begun) — the lock so it does not race with closing.
    void drain_listen(int fd) {
        FiberScheduler* s = get_thread_scheduler();
        if (!s) return;
        std::lock_guard<std::mutex> lk(lfd_mtx);
        if (std::find(listen_fds.begin(), listen_fds.end(), fd) == listen_fds.end()) return;
        for (int k = 0; k < 64 && running; ++k) {
            sockaddr_storage peer{};
            socklen_t peer_len = sizeof(peer);
            int client = accept4(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len, 0);
            if (client < 0) { if (errno == EINTR) continue; break; }   // EAGAIN: queue empty
            char peer_ip[INET6_ADDRSTRLEN] = {};
            if (peer.ss_family == AF_INET)
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, peer_ip, sizeof(peer_ip));
            else if (peer.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr, peer_ip, sizeof(peer_ip));
            std::string ip_str = peer_ip;
            try { s->spawn([this, client, ip_str]() { handle_connection(client, ip_str); }); }
            catch (...) { ::close(client); }
        }
    }
#endif

    // Per-worker listening sockets (fiber mode, SO_REUSEPORT). Tracked so begin_shutdown can
    // release the port immediately; the worker removes its own socket from the registry under this lock and closes it.
    std::mutex lfd_mtx;
    std::vector<int> listen_fds;

    // Stuck worker's socket (fiber mode). With SO_REUSEPORT the kernel assigns a new connection to a socket
    // without checking whether the worker is busy; if the worker is inside an indivisible builtin (large
    // json::decode, sort …) the connection that landed on that socket is not accepted until the work finishes (measured: /ok p99
    // 310 ms; only in builtins, since the VM loop yields every 1 ms). The watcher (like Go's
    // sysmon) has an idle worker drain the waiting connections of the stuck worker.
    // Does nothing under normal load. Registration is under lfd_mtx; the worker removes itself on exit.
    struct Watched {
        FiberScheduler* sched;
        int fd;
        std::shared_ptr<std::atomic<bool>> draining = std::make_shared<std::atomic<bool>>(false);
    };
    std::vector<Watched> watched;


    std::unique_ptr<WorkerPool> pool;

    // Event loop — only for WS/SSE async frame reading
    std::unique_ptr<EventLoop> loop;
    std::thread loop_thread;

    // WS state
    std::unordered_map<int, std::shared_ptr<WsConnection>> ws_clients;
    std::unordered_map<int, std::string>                   ws_bufs;
    std::mutex ws_mtx;

    // SSE state
    std::unordered_map<int, std::shared_ptr<SseConnection>> sse_clients;
    std::mutex sse_mtx;

    // Maximum request body size (bytes) — set via LOOK_MAX_BODY_SIZE,
    // default 10 MB. An unbounded body would allow a memory-exhaustion DoS.
    static size_t max_body_size() {
        static const size_t v = []() -> size_t {
            const char* e = std::getenv("LOOK_MAX_BODY_SIZE");
            if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
            return 10 * 1024 * 1024;
        }();
        return v;
    }

    // Header-only short response — for the paths that reject before reading the body.
    static void send_simple(int fd, int code, const char* reason) {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
#ifdef _WIN32
        ::send(fd, r.data(), (int)r.size(), 0);
#else
        ssize_t n = ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        (void)n;
#endif
    }

    // ── Full send (fiber-aware) ───────────────────────────────────────────
    // Writes until everything is sent; true = all of it went out. Two old bugs:
    //   * a single ::send() was made and a short write counted as success: with a slow-reading client part of the
    //     response went out, the connection stayed open, and the next response was read on the same connection as
    //     the continuation of the first one's body (Content-Length 8 MB, 1.8 MB received + next "HTTP/1.1 200 OK");
    //   * in fiber mode the socket was blocking: a SINGLE client that does not read froze the worker thread
    //     (with up to 1024 fibers on it) for SO_SNDTIMEO once the send buffer filled.
    // Inside a fiber, if the socket is full the fiber is suspended until writable (the thread is free);
    // if it cannot write within timeout_ms, false (connection is closed). Outside a fiber, a blocking write.
    static bool send_all(int fd, const char* p, size_t n, int timeout_ms) {
#ifdef __linux__
        look::FiberScheduler* sched = look::get_thread_scheduler();
        look::Fiber*          cur   = look::Fiber::current();
        std::shared_ptr<look::Fiber> self = (sched && cur) ? cur->shared_from_this_fiber() : nullptr;
        while (n > 0) {
            ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL | (self ? MSG_DONTWAIT : 0));
            if (w > 0) { p += w; n -= (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && self && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (sched->wait_writable_tmo(self, fd, timeout_ms) == 1) continue;
                return false;   // could not write (timeout / no epoll)
            }
            return false;       // error or SO_SNDTIMEO expired
        }
        return true;
#else
        while (n > 0) {
            int w = ::send(fd, p, (int)n, 0);
            if (w <= 0) return false;
            p += w; n -= (size_t)w;
        }
        return true;
#endif
    }

    // ── Fiber-aware recv (Go netpoller) ──────────────────────────────────────
    // Inside a fiber, if the socket has no data it does NOT BLOCK: yields with wait_readable,
    // the scheduler runs another fiber (goroutine); epoll resumes when data arrives.
    // Previously a plain ::recv() blocked the worker thread → while waiting for the next
    // request on keep-alive every worker was locked and other connections starved
    // (the ROOT CAUSE of the fiber dispatch c=100 hang). Outside a fiber, a plain blocking recv.
    static ssize_t fiber_aware_recv(int fd, char* buf, size_t len) {
#ifdef __linux__
        look::FiberScheduler* sched = look::get_thread_scheduler();
        look::Fiber*          cur   = look::Fiber::current();
        if (sched && cur) {
            auto self = cur->shared_from_this_fiber();
            while (true) {
                ssize_t r = ::recv(fd, buf, len, MSG_DONTWAIT);
                if (r >= 0) return r;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No data → yield. wait_readable_tmo parks on epoll + a 15s idle
                    // limit (D Fix #2): on keep-alive, if the client never sends the next request
                    // the fiber does not hang INDEFINITELY — on timeout we return 0
                    // (like peer-closed) → the connection closes, the fiber ends. The fix for the ab -k
                    // wind-down stall and for fiber accumulation at shutdown.
                    if (self) {
                        int w = sched->wait_readable_tmo(self, fd, 15000);
                        if (w == 1) continue;      // readable — try recv again
                        if (w == 0) return 0;      // idle timeout → close the connection
                    }
                    return ::recv(fd, buf, len, 0);  // no epoll → blocking fallback
                }
                return r;  // real error
            }
        }
#endif
        return ::recv(fd, buf, len, 0);  // non-fiber: blocking (existing behavior)
    }

    // Decode a Transfer-Encoding: chunked body (RFC 7230 §4.1).
    // buf: headers + the first part of the body; start: body offset.
    // The decoded body is written to out. Includes body cap and malformed checks.
    // false → request rejected (response sent), the caller must close the fd.
    // Body-read deadline. LOOK_HEADER_TIMEOUT only covers the header phase; without a
    // body deadline a slow-body attacker (a full header + large Content-Length, then a
    // byte every few seconds) holds a worker forever in pool mode — a proven DoS. This
    // is the header-timeout logic applied to the body loop. LOOK_BODY_TIMEOUT = total
    // deadline (ms, default 30s). LOOK_BODY_MIN_RATE = required average bytes/sec after a
    // 5s grace (0 = off — the gentler option that does not penalise large-but-fast
    // uploads; raise LOOK_BODY_TIMEOUT and set a floor instead). Either trips → close.
    static bool body_read_too_slow(std::chrono::steady_clock::time_point body_start,
                                   size_t received) {
        static const long tmo_ms = []() -> long {
            const char* e = std::getenv("LOOK_BODY_TIMEOUT");
            long ms = (e && *e) ? std::atol(e) : 30000;
            return ms > 0 ? ms : 30000;
        }();
        static const long min_rate = []() -> long {
            const char* e = std::getenv("LOOK_BODY_MIN_RATE");
            long r = (e && *e) ? std::atol(e) : 0;
            return r > 0 ? r : 0;
        }();
        long elapsed_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - body_start).count();
        if (elapsed_ms > tmo_ms) return true;                    // total deadline
        if (min_rate > 0 && elapsed_ms > 5000)                   // min-rate after 5s grace
            if ((double)received < (elapsed_ms / 1000.0) * (double)min_rate) return true;
        return false;
    }

    // rest: bytes after the body (the next request in the same send — pipelining)
    static bool read_chunked_body(int fd, std::string& buf, size_t start,
                                  char* tmp, size_t tmp_sz, std::string& out, std::string& rest) {
        std::string stream = buf.substr(start);   // body bytes (not yet decoded)
        size_t p = 0;
        const size_t cap    = max_body_size();
        const size_t raw_lim = cap + 1024 * 1024; // raw stream ceiling (chunk header allowance)

        // Body-read deadline — a slow-body drip can hold the worker on the chunked path too.
        auto body_start = std::chrono::steady_clock::now();
        bool timed_out = false;
        auto refill = [&]() -> bool {
            ssize_t r = fiber_aware_recv(fd, tmp, tmp_sz);
            if (r <= 0) return false;
            if (stream.size() + (size_t)r > raw_lim) return false; // stream blow-up
            stream.append(tmp, (size_t)r);
            if (body_read_too_slow(body_start, stream.size())) { timed_out = true; return false; }
            return true;
        };

        while (true) {
            // 1) get the chunk size line (hex, there may be a chunk-ext after ';')
            size_t nl;
            while ((nl = stream.find("\r\n", p)) == std::string::npos) {
                if (stream.size() - p > 64) { send_simple(fd, 400, "Bad Request"); return false; }
                if (!refill()) { send_simple(fd, timed_out ? 408 : 400, timed_out ? "Request Timeout" : "Bad Request"); return false; }
            }
            std::string sizeline = stream.substr(p, nl - p);
            // no control characters in the line including chunk-ext (except HTAB). Previously the ext was not checked
            // before being discarded: "3;a\nb\r\n" was accepted — a proxy that treats a bare LF as a line end
            // would frame the same stream differently (smuggling). The same as the strict CRLF rule in the header block.
            if (!http_field_value_ok(sizeline)) { send_simple(fd, 400, "Bad Request"); return false; }
            size_t semi = sizeline.find(';');            // discard the chunk-ext
            if (semi != std::string::npos) sizeline = sizeline.substr(0, semi);
            // hex parse (strict)
            if (sizeline.empty() ||
                sizeline.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                send_simple(fd, 400, "Bad Request"); return false;
            }
            size_t chunk_sz = 0;
            try { chunk_sz = std::stoull(sizeline, nullptr, 16); }
            catch (...) { send_simple(fd, 400, "Bad Request"); return false; }
            p = nl + 2;

            if (chunk_sz == 0) break;                    // last chunk
            // Overflow-safe: `out.size() + chunk_sz` could wrap with chunk_sz ~2^64 (hex
            // fffffffffffffffb) and skip the cap check.
            // Given the out.size() <= cap invariant, `cap - out.size()` is safe;
            // this also bounds chunk_sz to ≤cap and prevents the `p+chunk_sz+2`
            // overflow at line 280.
            if (chunk_sz > cap - out.size()) { send_simple(fd, 413, "Payload Too Large"); return false; }

            // 2) read until the chunk data + closing CRLF arrive
            while (stream.size() < p + chunk_sz + 2) {
                if (!refill()) { send_simple(fd, timed_out ? 408 : 400, timed_out ? "Request Timeout" : "Bad Request"); return false; }
            }
            out.append(stream, p, chunk_sz);
            p += chunk_sz;
            if (stream.compare(p, 2, "\r\n") != 0) { send_simple(fd, 400, "Bad Request"); return false; }
            p += 2;
        }
        // swallow trailer headers: up to the empty line (\r\n). Consumed from the stream so
        // the next request's boundary is correct on keep-alive.
        // If the connection closes before the empty line the request is INCOMPLETE → 400 (previously accepted:
        // "0\r\n" + EOF counted as a complete request; a proxy in front could frame it differently).
        size_t trailers = 0;
        while (true) {
            size_t nl2 = stream.find("\r\n", p);
            if (nl2 == std::string::npos) {
                if (stream.size() - p > 8192) { send_simple(fd, 400, "Bad Request"); return false; }
                if (!refill()) { send_simple(fd, timed_out ? 408 : 400, timed_out ? "Request Timeout" : "Bad Request"); return false; }
                continue;
            }
            if (nl2 == p) { p += 2; break; }             // empty line → body done
            // Trailer = header field rules (Go textproto): "name: value", name a token, no CTL in value.
            // Previously lines were skipped without inspection (bare LF, lines without ':' passed).
            {
                std::string tl = stream.substr(p, nl2 - p);
                size_t tc = tl.find(':');
                if (tc == std::string::npos || !http_token(tl.substr(0, tc)) ||
                    !http_field_value_ok(tl.substr(tc + 1))) {
                    send_simple(fd, 400, "Bad Request"); return false;
                }
            }
            if (++trailers > HTTP_MAX_HEADER_FIELDS) { send_simple(fd, 431, "Request Header Fields Too Large"); return false; }
            p = nl2 + 2;                                  // skip the trailer line
        }
        rest.assign(stream, p, std::string::npos);
        return true;
    }

    // ── Blocking HTTP connection handler — runs on a worker thread ──────────
    //
    // For each connection:
    //   1. blocking recv() → read until headers are complete
    //   2. parse → detect WS/SSE upgrade
    //   3. HTTP: handler → send() → keep-alive loop
    //   4. WS/SSE: hand over to the event loop, the worker is freed
    //
    // No event loop — same model as Apache.
    void handle_connection(int fd, std::string remote_addr = "") {
        // Per-IP concurrent-connection ceiling (opt-in, LOOK_HTTP_MAX_CONNS_IP). If exceeded,
        // send 429 and close — this IP already holds worker(s), do not take another.
        if (!http_ip_acquire(remote_addr)) {
            send_simple(fd, 429, "Too Many Requests"); ::close(fd); return;
        }
        // RAII: the slot is released however handle_connection returns (early close, WS/SSE
        // handover, end of keep-alive) — worker-scope semantics.
        struct IpSlot {
            const std::string& ip;
            // A connection upgraded to WS/SSE carries the slot with it (close_ws / close_sse release it):
            // releasing here and re-acquiring there counted the connection TWICE for a short time, and an address
            // one below the limit got an unjustified 429.
            bool moved = false;
            ~IpSlot() { if (!moved) http_ip_release(ip); }
        } ip_slot{ remote_addr };

        // Idle timeout — close if the client does not respond for N ms (default 30s). Configurable
        // via LOOK_HTTP_IDLE_MS (for test speed). PLATFORM-CORRECT TYPE: Windows SO_RCVTIMEO expects a
        // DWORD of milliseconds, POSIX expects a timeval. OLD BUG: timeval{30,0} was
        // passed on Windows → Winsock read the first DWORD (=30) and turned 30s into 30ms (idle
        // keep-alive dropped at ~30ms on Win). Measured empirically (~34ms), fixed.
        static const long idle_ms = []{
            const char* e = std::getenv("LOOK_HTTP_IDLE_MS");
            return (e && *e) ? std::atol(e) : 30000L;
        }();
#ifdef _WIN32
        DWORD tv = (DWORD)idle_ms;
#else
        timeval tv{ (time_t)(idle_ms/1000), (suseconds_t)((idle_ms%1000)*1000) };
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#ifndef _WIN32
        // Nagle off — small packets are sent immediately
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
#endif

        char tmp[65536];

        // Slowloris defense: after a request has STARTED (first byte) the headers
        // must complete within this total time. The idle timeout (15s/30s) is reset on every byte
        // so it is not enough alone — a slow drip (a byte per 6s, under idle) holds
        // the connection indefinitely and exhausts worker threads in pool mode (DoS).
        // Set with LOOK_HEADER_TIMEOUT ms (default 15s). On keep-alive, the idle
        // wait BETWEEN requests is NOT included (the timer starts at the first byte).
        static const auto HDR_TMO = []() -> std::chrono::milliseconds {
            const char* e = std::getenv("LOOK_HEADER_TIMEOUT");
            long ms = (e && *e) ? std::atol(e) : 15000;
            if (ms <= 0) ms = 15000;
            return std::chrono::milliseconds(ms);
        }();

        // Bytes that arrived after the previous request's body in the same send (HTTP/1.1 pipelining:
        // the client writes the second request without waiting for the response). Previously every request started with a fresh buffer
        // → the second request was silently dropped, and the client hung until the idle timeout waiting for a response.
        std::string carry;
        bool after_post = false;   // the previous request on this connection was a POST
        while (true) {
            // Read blocking until headers arrive
            std::string buf = std::move(carry);
            carry.clear();
            std::chrono::steady_clock::time_point rd_start{};   // set at the first byte
            if (!buf.empty()) rd_start = std::chrono::steady_clock::now();
            // The terminator is searched only in the NEWLY arrived part (+3 bytes overlap): previously on every recv
            // the whole buffer was rescanned from the start → a 2 MB header in 64 B pieces cost ~3 CPU-seconds (quadratic).
            size_t scan = 0, hdr_pos;
            // After a POST, up to 4 CR/LF bytes before the next request line are skipped — Go's tolerance for
            // clients that end a POST body with an extra CRLF (net/http conn.readRequest). Such a client used
            // to get its NEXT request answered with 400. Nowhere else: a request that starts with an empty
            // line is 400, as in Go (the same framing as a Go front end).
            size_t crlf_budget = after_post ? 4 : 0;
            after_post = false;
            auto skip_empty_lines = [&]() {
                size_t k = 0;
                while (crlf_budget && k < buf.size() && (buf[k] == '\r' || buf[k] == '\n')) { ++k; --crlf_budget; }
                if (k) { buf.erase(0, k); scan = 0; }
                if (!buf.empty()) crlf_budget = 0;          // the request line has started
            };
            skip_empty_lines();
            while ((hdr_pos = buf.find("\r\n\r\n", scan)) == std::string::npos) {
                scan = buf.size() >= 3 ? buf.size() - 3 : 0;
                // Waiting for the next request (no bytes yet): this wait is cut short on graceful shutdown.
                const bool waiting = buf.empty();
                if (waiting && !idle_enter(fd)) { ::close(fd); return; }
                ssize_t r = fiber_aware_recv(fd, tmp, sizeof(tmp));
                if (waiting) idle_leave(fd);
                if (r <= 0) { ::close(fd); return; }
                if (rd_start == std::chrono::steady_clock::time_point{})
                    rd_start = std::chrono::steady_clock::now();
                buf.append(tmp, (size_t)r);
                skip_empty_lines();
                // Go MaxHeaderBytes (1 MB) → 431. Previously closed without a response at 2 MB. The loop ceiling is
                // one read allowance wider (the last read can bring the start of the body too); the exact limit is
                // enforced by the parser against the end of the headers.
                if (buf.size() > HTTP_MAX_HEADER_BYTES + sizeof(tmp)) {
                    send_simple(fd, 431, "Request Header Fields Too Large"); ::close(fd); return;
                }
                if (std::chrono::steady_clock::now() - rd_start > HDR_TMO) {
                    send_simple(fd, 408, "Request Timeout"); ::close(fd); return;
                }
            }

            HttpRequest req;
            req.remote_addr = remote_addr;
            // Rejected request: write a status response like Go, then close (previously closed without a response —
            // the client and proxy only saw "connection dropped", with no way to know why it was 400).
            if (int st = parse_request(buf, req)) {
                send_simple(fd, st, http_status_reason(st)); ::close(fd); return;
            }
            after_post = (req.method == "POST");

            size_t header_end  = hdr_pos + 4;
            size_t body_in_buf = buf.size() - header_end;
            auto clen_it = req.headers.find("content-length");
            auto te_it   = req.headers.find("transfer-encoding");

            // HTTP Request Smuggling protection: if Content-Length and Transfer-Encoding
            // arrive together the request is ambiguous (RFC 7230 §3.3.3) — strict rejection.
            // (Go deletes Content-Length here and continues; we stay strict — smuggling defense.)
            if (te_it != req.headers.end() && clen_it != req.headers.end()) {
                send_simple(fd, 400, "Bad Request"); ::close(fd); return;
            }

            // Expect (RFC 9110 §10.1.1, Go): "100-continue" → "100 Continue" before the body is read
            // (previously not written: clients like curl waited ~1 s before sending the body);
            // unknown expectation → 417. 100 is written only for HTTP/1.1 and if the body has not arrived yet.
            static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
            bool want_continue = false;
            {
                auto ex = req.headers.find("expect");
                if (ex != req.headers.end()) {
                    std::string ev = ex->second;
                    std::transform(ev.begin(), ev.end(), ev.begin(), ::tolower);
                    if (ev != "100-continue") { send_simple(fd, 417, "Expectation Failed"); ::close(fd); return; }
                    want_continue = (req.version == "HTTP/1.1");
                }
            }

            // ── Transfer-Encoding: chunked body decoding ────────────────────────
            if (te_it != req.headers.end()) {
                std::string te = te_it->second;
                std::transform(te.begin(), te.end(), te.begin(), ::tolower);
                // RFC 7230 §3.3.1: TE is a token list; "chunked" must be the final coding.
                // Previously `find("chunked")` matched a SUBSTRING → "xchunked",
                // "chunkedx", "chunked, gzip" were all taken as chunked (measured:
                // 200/blen=5). If a front-end frames these differently there is desync
                // (request smuggling). LOOK supports only chunked → EXACT match:
                // reject if the value is not exactly "chunked" (gzip/compress cannot be decoded anyway).
                // Unsupported coding: 501 like Go (previously 400 — the request is not malformed, the server
                // does not support that coding).
                if (te != "chunked") {
                    send_simple(fd, 501, "Not Implemented"); ::close(fd); return;
                }
                if (want_continue && body_in_buf == 0 &&
                    !send_all(fd, k100, sizeof(k100) - 1, (int)idle_ms)) { ::close(fd); return; }
                if (!read_chunked_body(fd, buf, header_end, tmp, sizeof(tmp), req.body, carry)) {
                    // malformed chunk or body cap exceeded → read_chunked_body already sent the response
                    ::close(fd); return;
                }
            } else if (clen_it != req.headers.end()) {
                // Safe Content-Length parse — a malformed/overflowing value must not
                // take the worker down (std::stoul threw an exception = DoS).
                size_t content_len = 0;
                {
                    const std::string& cl = clen_it->second;
                    if (cl.empty() || cl.size() > 19 ||
                        cl.find_first_not_of("0123456789") != std::string::npos) {
                        send_simple(fd, 400, "Bad Request"); ::close(fd); return;
                    }
                    try { content_len = std::stoull(cl); }
                    catch (...) { send_simple(fd, 400, "Bad Request"); ::close(fd); return; }
                }
                // Maximum body size — an unbounded body is a memory-exhaustion DoS.
                // Set with LOOK_MAX_BODY_SIZE (bytes), default 10 MB.
                if (content_len > max_body_size()) {
                    send_simple(fd, 413, "Payload Too Large"); ::close(fd); return;
                }
                if (want_continue && body_in_buf < content_len &&
                    !send_all(fd, k100, sizeof(k100) - 1, (int)idle_ms)) { ::close(fd); return; }
                if (body_in_buf > 0)
                    req.body = buf.substr(header_end,
                                          std::min(body_in_buf, content_len));
                if (body_in_buf > content_len)   // after the body: the next request
                    carry.assign(buf, header_end + content_len, std::string::npos);
                // Body-read deadline — closes slow-body worker starvation (DoS).
                auto body_start = std::chrono::steady_clock::now();
                while (req.body.size() < content_len) {
                    ssize_t r = fiber_aware_recv(fd, tmp, sizeof(tmp));
                    if (r <= 0) { ::close(fd); return; }
                    if (body_read_too_slow(body_start, req.body.size())) {
                        send_simple(fd, 408, "Request Timeout"); ::close(fd); return;
                    }
                    // Content-Length is a HARD upper bound (bug 55): if the body arrives in a separate segment
                    // and the last recv brings extra bytes (the start of the next
                    // pipelined request), do NOT swallow those bytes into the body — otherwise the body
                    // exceeds CL (the app sees a corrupt body + smuggling-adjacent: the next
                    // request gets mixed into this body). The first-buffer path (above) already
                    // clipped with min(); this loop did not.
                    size_t need = content_len - req.body.size();
                    req.body.append(tmp, std::min((size_t)r, need));
                    if ((size_t)r > need) carry.assign(tmp + need, (size_t)r - need);   // next request
                }
            } else if (body_in_buf > 0) {
                // Bodyless request (no CL/TE → body is 0 bytes, RFC 9112 §6.3): what follows the headers
                // is the next request.
                carry.assign(buf, header_end, std::string::npos);
            }

            // WS upgrade → hand over to the event loop, the worker is freed
            if (req.upgrade_websocket && ws_handler) {
                handle_ws_upgrade(fd, req, ip_slot.moved);
                return;
            }

            // SSE upgrade → hand over to the event loop
            if (req.upgrade_sse && sse_handler) {
                handle_sse_upgrade(fd, req, ip_slot.moved);
                return;
            }

            // Keep-alive detection (HTTP/1.1 default = keep-alive)
            bool req_keep_alive = (req.version == "HTTP/1.1");
            {
                auto ci = req.headers.find("connection");
                if (ci != req.headers.end()) {
                    std::string cv = ci->second;
                    std::transform(cv.begin(), cv.end(), cv.begin(), ::tolower);
                    if (cv.find("close")      != std::string::npos) req_keep_alive = false;
                    if (cv.find("keep-alive") != std::string::npos) req_keep_alive = true;
                }
            }

            // "OPTIONS *" (about the server itself; the parser accepts '*' only for OPTIONS):
            // like Go's globalOptionsHandler, 200 with no body without going to the app. Previously the "*" path looked up
            // a route and returned 404.
            if (req.path == "*") {
                HttpResponse o;
                o.keep_alive = req_keep_alive;
                std::string raw = o.build();
                if (!send_all(fd, raw.data(), raw.size(), (int)idle_ms) || !o.keep_alive) { ::close(fd); return; }
                continue;
            }

            // Dispatch — the worker thread handles it directly
            HttpResponse resp;
            resp.keep_alive = req_keep_alive;
            resp.headers["Content-Type"] = "application/json; charset=utf-8";
            try {
                handler(req, resp);
            } catch (...) {
                resp.status_code = 500;
                resp.status_text = "Internal Server Error";
                // "hata" is a deprecated copy of "error": removed together with the CGI/FastCGI "hata"/"kod".
                resp.body        = "{\"ok\":false,\"error\":\"Server error\",\"hata\":\"Server error\"}";
                resp.keep_alive  = false;
            }
            // Shutdown in progress (started while the request was being handled): the response goes with "Connection: close",
            // the connection closes — so the client does not send its next request to a process that is closing (Go Shutdown).
            if (draining.load(std::memory_order_acquire)) resp.keep_alive = false;

            std::string raw = resp.build();
            // If it does not all go out the connection closes: on a half-written response the next response is not written.
            if (!send_all(fd, raw.data(), raw.size(), (int)idle_ms) || !resp.keep_alive) { ::close(fd); return; }
            // keep-alive: wait for the next request
        }
    }

    // ── WebSocket upgrade — runs on a worker thread ──────────────────────────
    // Extract the host[:port] part from the Origin header (drop scheme:// and the path).
    // "host[:port]" → host (lowercase). IPv6 "[::1]:8080" → "[::1]": the port is trimmed only if the
    // ':' is AFTER the ']'. Previously the last ':' was always trimmed → portless "[::1]" and "[::2]"
    // both became "[:" (different IPv6 addresses counted as "same origin").
    static std::string ws_strip_port(std::string h) {
        if (!h.empty() && h[0] == '[') {
            auto rb = h.find(']');
            if (rb != std::string::npos) h.resize(rb + 1);
        } else {
            auto colon = h.rfind(':');
            if (colon != std::string::npos) h.resize(colon);
        }
        for (auto& ch : h) ch = (char)std::tolower((unsigned char)ch);   // host is case-insensitive
        return h;
    }
    static std::string ws_origin_host(const std::string& origin) {
        auto p = origin.find("://");
        std::string h = (p == std::string::npos) ? origin : origin.substr(p + 3);
        auto slash = h.find('/');
        if (slash != std::string::npos) h = h.substr(0, slash);
        return ws_strip_port(h);              // compare by host
    }

    // RFC 6455 §4.2.1: Sec-WebSocket-Key = base64 of 16 bytes (24 characters, ends with "==").
    static bool ws_key_valid(const std::string& k) {
        if (k.size() != 24 || k[22] != '=' || k[23] != '=') return false;
        for (size_t i = 0; i < 22; i++) {
            char c = k[i];
            if (!std::isalnum((unsigned char)c) && c != '+' && c != '/') return false;
        }
        return true;
    }

    void handle_ws_upgrade(int fd, const HttpRequest& req, bool& ip_slot_moved) {
        // A handshake with a missing/malformed key or a non-GET does not get 101 (previously even an empty key
        // got 101 → half-handshaken connections occupied space in the event loop).
        if (req.method != "GET" || !ws_key_valid(req.ws_key)) {
            send_simple(fd, 400, "Bad Request"); ::close(fd); return;
        }
        // CSWSH (Cross-Site WebSocket Hijacking) protection.
        auto it = req.headers.find("origin");
        if (!allowed_origins.empty()) {
            // An explicit allowlist is configured (LOOK_WS_ORIGINS) — exact match.
            if (it == req.headers.end()) {
                send_ws_reject(fd, "403 Forbidden - Origin header required");
                ::close(fd);
                return;
            }
            const std::string& origin = it->second;
            bool ok = false;
            for (const auto& allowed : allowed_origins)
                if (allowed == "*" || allowed == origin) { ok = true; break; }
            if (!ok) {
                send_ws_reject(fd, "403 Forbidden - Origin not allowed: " + origin);
                ::close(fd);
                return;
            }
        } else if (it != req.headers.end()) {
            // No allowlist → safe default: enforce SAME-ORIGIN. A browser
            // request (Origin header present) is accepted only if its host matches the Host header;
            // a cross-origin browser request (the real CSWSH vector) is
            // rejected. Requests WITHOUT an Origin header (backend services, non-browser
            // clients like wscat) are unaffected — no cross-site risk.
            // To go back to allow-all: LOOK_WS_ORIGINS=*
            std::string oh = ws_origin_host(it->second);
            auto host_it = req.headers.find("host");
            std::string hh;
            if (host_it != req.headers.end())
                hh = ws_strip_port(host_it->second);    // "host[:port]"
            if (oh.empty() || hh.empty() || oh != hh) {
                send_ws_reject(fd, "403 Forbidden - Cross-origin WebSocket rejected "
                                   "(same-origin default; use LOOK_WS_ORIGINS to allow)");
                ::close(fd);
                return;
            }
        }
        if (!held_ip_acquire(req.remote_addr)) { send_simple(fd, 429, "Too Many Requests"); ::close(fd); return; }
        std::string hs = ws_handshake_101(req.ws_key);
#ifdef _WIN32
        ::send(fd, hs.data(), (int)hs.size(), 0);
#else
        ::send(fd, hs.data(), hs.size(), MSG_NOSIGNAL);
#endif
        auto conn = std::make_shared<WsConnection>(fd);
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            ws_clients[fd] = conn;
            ws_bufs[fd]    = "";
        }
        look::g_ws_registry.add(conn);
        conn->held_ip = req.remote_addr; ip_slot_moved = true;

        ws_handler(conn, req);

        // Hand the fd to the event loop: blocking → non-blocking + EPOLL_CTL_ADD. Registration happens in the event loop,
        // on the same thread as close (close_ws): if the handler called ws::close, the close runs first
        // and the connection is no longer on this fd → it is not registered. Previously the worker registered: while the loop
        // closed the fd, epoll_ctl could go to the same fd (or a reused fd of another connection).
        loop->post([this, fd, conn]() {
            { std::lock_guard<std::mutex> lk(ws_mtx); auto it = ws_clients.find(fd); if (it == ws_clients.end() || it->second != conn) return; }
            loop->add_client(fd, [this, fd](const char* d, size_t l) { on_ws_data(fd, d, l); });
        });
    }

    // ── SSE upgrade — runs on a worker thread ───────────────────────────────
    void handle_sse_upgrade(int fd, const HttpRequest& req, bool& ip_slot_moved) {
        if (!held_ip_acquire(req.remote_addr)) { send_simple(fd, 429, "Too Many Requests"); ::close(fd); return; }
        const char* hdrs =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "X-Accel-Buffering: no\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
#ifdef _WIN32
        ::send(fd, hdrs, (int)strlen(hdrs), 0);
#else
        ::send(fd, hdrs, strlen(hdrs), MSG_NOSIGNAL);
#endif
        auto conn = std::make_shared<SseConnection>(fd);
        // sse::close (worker thread) → run close_sse on the loop thread (fd/epoll/map
        // cleanup belongs to the loop). Without this bridge sse::close only marked closed →
        // the resource leaked until the client's TCP dropped (measured).
        conn->request_loop_close = [this, fd]() { loop->post([this, fd]() { close_sse(fd); }); };
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            sse_clients[fd] = conn;
        }
        look::g_sse_registry.add(conn);
        conn->held_ip = req.remote_addr; ip_slot_moved = true;

        sse_handler(conn, req);

        // Hand the fd to the event loop — for disconnect detection. Same as WS: registration in the event loop,
        // ordered with close_sse (not registered if the handler called sse::close — TSan: close_fd ↔ epoll_ctl).
        loop->post([this, fd, conn]() {
            { std::lock_guard<std::mutex> lk(sse_mtx); auto it = sse_clients.find(fd); if (it == sse_clients.end() || it->second != conn) return; }
            loop->add_client(fd, [this, fd](const char* d, size_t l) { on_sse_data(fd, d, l); });
        });
    }

    // ── SSE disconnect ────────────────────────────────────────────────────────
    void on_sse_data(int fd, const char* /*data*/, size_t len) {
        bool do_close = (len == 0);
        if (!do_close) {
            std::lock_guard<std::mutex> lk(sse_mtx);
            auto it = sse_clients.find(fd);
            do_close = (it == sse_clients.end() || it->second->closed.load());
        }
        if (do_close) { close_sse(fd); return; }
        loop->async_read(fd, [this, fd](const char* d, size_t l) {
            on_sse_data(fd, d, l);
        });
    }

    void close_sse(int fd) {
        std::shared_ptr<SseConnection> conn;
        bool owned = false;
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            auto it = sse_clients.find(fd);
            if (it != sse_clients.end()) { conn = it->second; sse_clients.erase(it); owned = true; }
        }
        // OWN-ONCE: the ONLY call that erases this fd from the map does the cleanup. There are two close_sse paths —
        // client-FIN (on_sse_data read-event) and sse::close (loop->post). Both run
        // in order on the loop thread; the first takes ownership, the second leaves here. The old code still called
        // `loop->close_fd(fd)` when conn was not found → DOUBLE close_fd = closing the WRONG fd
        // if the fd had been reused. That dangerous branch was removed (structural via own-once).
        if (!owned) return;
        held_ip_release(conn->held_ip);
        look::g_sse_registry.remove(fd);
        bool fire_cb = false;
        {
            fire_cb = !conn->closed.exchange(true);
            // DEFENSE (mirror of close_ws's 0525ba7 drain): take write_mutex BEFORE close_fd.
            // If the handler DEFERS send with timer::every / parallel (the natural SSE push pattern),
            // that callback calls SseConnection::send() on a SEPARATE thread: it checks closed under write_mutex
            // and then does ::send(fd). Without the drain: while send() is between the closed check (sse.cpp:61)
            // and ::send(63), close_sse (event loop) does close_fd(fd) → use-after-close
            // (if the fd was reused, writing to the wrong client). write_mutex makes the in-flight send
            // wait; the next send sees closed=true.
            // NOTE: this is an fd-LIFETIME semantic race — on a kernel fd, not in user
            // memory → TSan CANNOT SEE it (the positive control is 0 in both variants).
            // Evidence: code logic + threading. Severity LOW-MEDIUM (not corruption, a wrong-fd write).
            std::lock_guard<std::mutex> wlk(conn->write_mutex);
            loop->close_fd(fd);   // own-once → exactly once (no double close)
        }
        if (fire_cb) {
            if (auto cb = conn->get_on_close_cb()) pool->submit([cb]() { cb(); });
        }
    }

    // ── WS callbacks: in order per connection ──────────────────────────
    // Work goes into the connection's inbox; if no job is draining the inbox, one is given to the pool.
    // The drainer runs jobs one at a time; after WS_DRAIN_BATCH jobs it requeues itself
    // so that a single chatty connection does not monopolize a pool thread.
    static constexpr int WS_DRAIN_BATCH = 64;
    // Backpressure limits (pending callbacks per connection): above the limit reading stops,
    // below it resumes. Message count is counted too: each 1-byte frame costs ~100 B of work.
    static constexpr size_t WS_INBOX_HIGH_MSGS = 1000, WS_INBOX_LOW_MSGS = 250;
    static constexpr size_t WS_INBOX_HIGH_BYTES = 16u << 20, WS_INBOX_LOW_BYTES = 4u << 20;
    void ws_in_order(const std::shared_ptr<WsConnection>& conn, std::function<void()> job, size_t bytes = 0) {
        bool start;
        {
            std::lock_guard<std::mutex> lk(conn->inbox_mtx);
            conn->inbox.push_back({std::move(job), bytes});
            conn->inbox_bytes += bytes;
            start = !conn->inbox_running;
            conn->inbox_running = true;
        }
        if (start) ws_drain(conn);
    }
    // In the event loop: if the inbox is full, stop reading (do not re-arm). true = paused.
    bool ws_pause_if_full(const std::shared_ptr<WsConnection>& conn) {
        std::lock_guard<std::mutex> lk(conn->inbox_mtx);
        if (conn->inbox.size() > WS_INBOX_HIGH_MSGS || conn->inbox_bytes > WS_INBOX_HIGH_BYTES) {
            conn->read_paused = true;
            // The read callback is persistent (edge-triggered, reads until EAGAIN): not re-arming
            // does not stop reading — the loop is told explicitly. Under the lock: so it does not race with the drainer's resume.
            loop->pause_read(conn->fd);
            return true;
        }
        return false;
    }
    void ws_arm_read(int fd) {
        loop->async_read(fd, [this, fd](const char* d, size_t l) { on_ws_data(fd, d, l); });
    }
    void ws_drain(std::shared_ptr<WsConnection> conn) {
        pool->submit([this, conn]() {
            for (int k = 0; k < WS_DRAIN_BATCH; ++k) {
                std::function<void()> job;
                bool resume = false;
                {
                    std::lock_guard<std::mutex> lk(conn->inbox_mtx);
                    if (conn->inbox.empty()) { conn->inbox_running = false; return; }
                    job = std::move(conn->inbox.front().fn);
                    conn->inbox_bytes -= conn->inbox.front().bytes;
                    conn->inbox.pop_front();
                    if (conn->read_paused && conn->inbox.size() <= WS_INBOX_LOW_MSGS &&
                        conn->inbox_bytes <= WS_INBOX_LOW_BYTES) {
                        conn->read_paused = false;
                        resume = true;
                    }
                }
                if (resume) {
                    // Re-arm reading in the event loop — if the connection is still on this fd (if it closed and the fd
                    // was reused, do not touch the other connection's read).
                    int fd = conn->fd;
                    loop->post([this, conn, fd]() {
                        bool same;
                        { std::lock_guard<std::mutex> lk(ws_mtx); auto it = ws_clients.find(fd); same = it != ws_clients.end() && it->second == conn; }
                        if (!same) return;
                        // If the connection closed while reading was paused (ws::send to a gone client failed →
                        // closed) do the close here: since reading is not armed, EOF is never read, close_ws
                        // is not called, the "close" callback does not run and the connection would leak.
                        if (conn->closed.load()) close_ws(fd);
                        else ws_arm_read(fd);
                    });
                }
                try { job(); } catch (...) {}   // the callback logs its own error; the inbox must not get stuck
            }
            ws_drain(conn);
        });
    }

    // ── WS frame I/O ─────────────────────────────────────────────────────
    void on_ws_data(int fd, const char* data, size_t len) {
        // len==0 = connection closed (event loop clean-FIN/error notification). Reap the registry
        // + buffer — otherwise on every clean disconnect ws_clients/ws_bufs/
        // g_ws_registry leak → when MAX_WS is reached new upgrades are permanently rejected
        // + broadcast keeps writing to a closed/reopened fd.
        if (len == 0) {
            // The client left WITHOUT SENDING a close frame (network dropped, tab closed, mobile sleep).
            // The "close" callback fired only on a close frame → on this path it never
            // ran, and the application's connection list/cleanup leaked (on SSE a drop already
            // fires). Rule: once when the client leaves; not if the server closed it itself (closed).
            close_ws(fd);   // the "close" callback is in close_ws (single place)
            return;
        }
        std::shared_ptr<WsConnection> conn;
        // The buffer is taken once in the event loop, decoded in a single pass from start to end, and the remainder put back
        // at the end. Previously the WHOLE buffer was copied and erased from the front on EVERY frame (quadratic: thousands of small frames
        // in a single read, or pieces of a large frame, locked up the event loop).
        std::string buf;
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_clients.find(fd);
            if (it == ws_clients.end()) return;
            conn = it->second;
            auto& b = ws_bufs[fd];
            b.append(data, len);
            buf.swap(b);
        }
        if (!conn || conn->closed.load()) { close_ws(fd); return; }

        size_t off = 0;
        while (true) {
            WsFrame frame = ws_try_decode_frame(std::string_view(buf).substr(off));
            if (frame.protocol_error) {
                // RFC 6455 §5.1: unmasked client frame → close with 1002
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                break;
            }
            if (!frame.complete) break;
            off += frame.consumed;

            // RFC 6455 §5.4 fragment reassembly + 1002 on §5.1 protocol violations.
            // binary: binary frame (0x2); for a fragmented message the opcode of the first frame (frag_opcode).
            // The limit is the same for both: frame 16 MB (ws_try_decode_frame), reassembled message 16 MB.
            auto ws_deliver = [&](const std::string& msg, bool binary) {
                if (auto cb = conn->get_on_message())   // locked copy: ws::on may assign at the same time
                    ws_in_order(conn, [cb, msg, binary]() { cb(msg, binary); }, msg.size() + 128);
            };
            auto ws_proto_close = [&]() {
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
            };
            // A text message must be valid UTF-8 (RFC 6455 §8.1): otherwise close with 1007, the handler
            // does not see it. OLD: corrupt bytes were delivered to the handler and could go from there to JSON/database/HTML
            // (since sending now rejects them, accepting on receive was inconsistent too).
            // For a fragmented message the reassembled message is checked (a character can be split across fragments).
            // A binary message is not checked.
            auto ws_text_ok = [&](const std::string& msg) -> bool {
                if (ws_utf8_valid(msg)) return true;
                std::string cf = ws_encode_close_frame_code(1007);
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                return false;
            };

            uint8_t op = frame.opcode;
            if (op == 0x00 || op == 0x01 || op == 0x02) {
                // Reassembled message cap — against fragment-accumulation DoS.
                static constexpr size_t WS_MAX_MESSAGE = 16 * 1024 * 1024;
                if (op == 0x00) {
                    // Continuation — there must be an active fragmented message (RFC §5.4).
                    if (!conn->frag_active) { ws_proto_close(); break; }
                    conn->frag_buf += frame.payload;
                } else {
                    // New text/binary — a new one cannot start before the previous fragment finishes.
                    if (conn->frag_active) { ws_proto_close(); break; }
                    if (frame.fin) {                                               // single frame
                        if (op == 0x01 && !ws_text_ok(frame.payload)) break;
                        ws_deliver(frame.payload, op == 0x02);
                        continue;
                    }
                    conn->frag_active = true;
                    conn->frag_opcode = op;
                    conn->frag_buf    = frame.payload;
                }
                if (conn->frag_buf.size() > WS_MAX_MESSAGE) { ws_proto_close(); break; }
                if (frame.fin) {                       // fragmented message complete
                    std::string msg = std::move(conn->frag_buf);
                    conn->frag_buf.clear();
                    conn->frag_active = false;
                    if (conn->frag_opcode == 0x01 && !ws_text_ok(msg)) break;
                    ws_deliver(msg, conn->frag_opcode == 0x02);
                }
            } else if (frame.opcode == 0x09) {
                std::string pong = ws_encode_pong_frame(frame.payload);
                std::lock_guard<std::mutex> lk(conn->write_mutex);
                conn->send_raw(pong);
            } else if (frame.opcode == 0x08) {
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                close_ws(fd);   // the "close" callback is in close_ws (single place)
                return;
            }
        }

        // The leftover (half frame) is put back — if the connection closed in the meantime there is no entry.
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_bufs.find(fd);
            if (it != ws_bufs.end()) it->second.assign(buf, off, std::string::npos);
        }
        if (!conn->closed.load()) {
            // Backpressure: if the inbox is full reading is not armed; the drainer arms it when it drops to the low watermark.
            if (!ws_pause_if_full(conn)) ws_arm_read(fd);
        } else {
            close_ws(fd);
        }
    }

    // After ws::close (WsConnection::close_conn): the close frame has gone out, close the connection in the
    // event loop. If the fd was reused in the meantime the new connection is not server_closed →
    // untouched (no identity comparison needed).
    void ws_server_closed(int fd) {
        loop->post([this, fd]() {
            std::shared_ptr<WsConnection> conn;
            { std::lock_guard<std::mutex> lk(ws_mtx); auto it = ws_clients.find(fd); if (it != ws_clients.end()) conn = it->second; }
            if (conn && conn->server_closed.load() && conn->closed.load()) close_ws(fd);
        });
    }

    void close_ws(int fd) {
        std::shared_ptr<WsConnection> conn;
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_clients.find(fd);
            if (it != ws_clients.end()) conn = it->second;
            ws_clients.erase(fd);
            ws_bufs.erase(fd);
        }
        // fd close-vs-send race (TSan-proven): while broadcast/send_raw does ::send(fd),
        // if close_fd(fd) closes the fd → a write to a closed/reused fd (cross-talk).
        // Fix: closed=true (no new send starts — send_raw/send_text closed check) THEN
        // DRAIN write_mutex (wait for in-flight sends to finish) — while write_mutex is held
        // no send_raw can be inside ::send → close_fd is safe. The lock also covers close_fd.
        if (conn) {
            // "close" callback — the SINGLE place: once however the connection ends (close frame,
            // drop, send error, protocol error); never if the server closed with ws::close.
            // Goes through the inbox: runs AFTER pending message callbacks.
            if (auto cb = conn->get_on_close(); cb && !conn->server_closed.load() && !conn->close_fired.exchange(true))
                ws_in_order(conn, [cb]() { cb(); });
            held_ip_release(conn->held_ip);
            conn->closed.store(true);
            std::lock_guard<std::mutex> wlk(conn->write_mutex);  // drain the in-flight send
            conn->fd_released = true;   // so a close_conn waiting on the lock does not write to a closed fd
            look::g_ws_registry.remove(fd);
            loop->close_fd(fd);
        } else {
            look::g_ws_registry.remove(fd);
            loop->close_fd(fd);
        }
    }
};

// ── HttpServer public API ─────────────────────────────────────────────────────

// Listen address of HTTP mode: --bind / LOOK_BIND (IPv4). Empty → all interfaces (the default
// is unchanged: the Ubuntu setup and Docker serve directly). Previously the address could not be chosen: a site behind
// a reverse proxy (Apache on Plesk) was also open to the internet on its own port — the proxy could be
// bypassed, disabling HTTPS, proxy rules and trusted proxy headers.
// A name (localhost) is not accepted: resolution may fall to ::1 and fail to match an IPv4 listener.
static uint32_t http_bind_addr() {
    const char* e = std::getenv("LOOK_BIND");
    if (!e || !*e) return htonl(INADDR_ANY);
    in_addr a{};
    if (inet_pton(AF_INET, e, &a) != 1)
        throw std::runtime_error(std::string("invalid --bind / LOOK_BIND address '") + e +
                                 "': give an IPv4 address such as 127.0.0.1 (a name like localhost is not resolved)");
    return a.s_addr;
}

// The process's HTTP server (single) — for the ws::close hook.
static std::atomic<void*> s_ws_close_impl{nullptr};
void HttpServer::ws_close_hook(int fd) {
    if (void* p = s_ws_close_impl.load()) static_cast<Impl*>(p)->ws_server_closed(fd);
}

HttpServer::HttpServer(int port, int workers, HttpHandler handler,
                       WsHandler ws_handler, SseHandler sse_handler)
    : impl_(std::make_unique<Impl>())
{
    s_ws_close_impl.store(impl_.get());
    g_ws_server_close_hook = &HttpServer::ws_close_hook;
    impl_->port        = port;
    impl_->workers     = workers;
    impl_->handler     = std::move(handler);
    impl_->ws_handler  = std::move(ws_handler);
    impl_->sse_handler = std::move(sse_handler);
    impl_->loop        = EventLoop::create();

    // CSWSH protection: LOOK_WS_ORIGINS=https://site.com,https://other.com
    if (const char* env = std::getenv("LOOK_WS_ORIGINS")) {
        std::string s = env;
        size_t pos = 0, found;
        while ((found = s.find(',', pos)) != std::string::npos) {
            impl_->allowed_origins.push_back(s.substr(pos, found - pos));
            pos = found + 1;
        }
        if (pos < s.size()) impl_->allowed_origins.push_back(s.substr(pos));
    }
    impl_->pool        = std::make_unique<WorkerPool>(workers);

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
#else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (srv < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = http_bind_addr();
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed on " + std::string(std::getenv("LOOK_BIND") && *std::getenv("LOOK_BIND") ? std::getenv("LOOK_BIND") : "0.0.0.0") + ":" + std::to_string(port));
    raise_fd_limit();
    for (const char* gone : {"LOOK_WS_MAX_CONN", "LOOK_SSE_MAX_CONN"}) {
        const char* e = std::getenv(gone);
        if (e && *e) look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
            std::string(gone) + " is set but is no longer used: there is no limit on the total of open connections. "
            "Limit connections per address with LOOK_HTTP_MAX_CONNS_IP.");
    }
    if (!fiber_dispatch_on()) {
        // Worker-per-connection model (LOOK_FIBER_DISPATCH=0 or a non-fiber platform, e.g.
        // Windows): an open connection holds a worker until it sends a request or times out. With no
        // proxy in front, as many open connections as workers (including idle browser connections) make everyone
        // wait (measured: 8 workers, 8 connections → 30 s). Do not stay silent.
        const char* tp = std::getenv("LOOK_TRUSTED_PROXY");
        if (!(tp && *tp))
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "Each open connection holds a worker until it sends a request or times out "
                "(worker-per-connection dispatch: LOOK_FIBER_DISPATCH=0, or a platform without fibers). "
                "With no reverse proxy in front, as many open connections as there are workers - idle "
                "browser connections included - make every other client wait. Put a reverse proxy in "
                "front and set LOOK_TRUSTED_PROXY, or set LOOK_HTTP_MAX_CONNS_IP.");
    }
    // listen() NOT HERE: the port is opened in run() once the workers are ready. Previously it listened here:
    // a client connecting between the constructor and run() (in fiber mode this socket is closed in run(); under the memory
    // limit the process cannot start workers and exits) had its connection accepted and left without a response (measured:
    // under load at 1024 MB, 17 of 40 exits with a stated reason had accepted a connection first). Bind stays here:
    // if the port is busy, the error is immediate.
    impl_->server_fd = srv;
}

// "Listening on …" only once the port is really listening — previously printed right after the constructor.
static void log_listening(int port) {
    static std::once_flag once;
    std::call_once(once, [port] {
        const char* b = std::getenv("LOOK_BIND");
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
            "Listening on " + std::string(b && *b ? b : "0.0.0.0") + ":" + std::to_string(port));
    });
}

HttpServer::~HttpServer() {
    void* me = impl_.get();
    s_ws_close_impl.compare_exchange_strong(me, nullptr);
    impl_->running = false;
    if (impl_->server_fd >= 0) ::close(impl_->server_fd);
}

void HttpServer::run() {
    impl_->running = true;

    // Event loop — in the background, only for WS/SSE
    impl_->loop_thread = std::thread([this]() {
        impl_->loop->run();
    });

    // SO_REUSEPORT + fiber burst-accept mode (LOOK_FIBER_DISPATCH=1, Linux only):
    //   Each worker opens its own SO_REUSEPORT socket — kernel distributes
    //   connections evenly across all workers (vs old model: single shared fd
    //   → one worker grabs everything → only 1 CPU active).
    //   Worker loop: poll → burst accept → spawn fibers → run_until_complete()
    static const bool fiber_burst = fiber_dispatch_on();

#ifdef __linux__
    // SO_REUSEPORT mode: per-worker sockets replace the shared fd.
    // Close shared socket now so its queue doesn't steal connections from workers.
    int shared_port = impl_->port;
    if (fiber_burst && impl_->server_fd >= 0) {
        ::close(impl_->server_fd);
        impl_->server_fd = -1;
    }
#endif

    // Startup gate (fiber mode): no worker starts listening until ALL are ready (socket bound, accept fiber
    // set up). Previously every worker listened on its socket immediately: when under a virtual memory
    // limit the next worker could not start its fiber and the process exited writing its reason, the port was
    // briefly open — a client connected and got no response (breaking the promise "either serve or say why").
    struct StartGate { std::mutex m; std::condition_variable cv; int ready = 0; bool abort = false; } gate;
    auto gate_arrive_and_wait = [&gate, this]() -> bool {
        std::unique_lock<std::mutex> lk(gate.m);
        if (++gate.ready >= impl_->workers) gate.cv.notify_all();
        gate.cv.wait(lk, [&] { return gate.abort || gate.ready >= impl_->workers; });
        return !gate.abort;
    };
    // Worker-per-connection model (no fiber / Windows): the shared socket is listened on the main thread
    // after ALL worker threads have started; workers wait here until then (accept on an
    // unopened socket returns immediately with EINVAL — they must not spin). If one cannot start the gate is cancelled: exit without listening.
    struct ListenGate { std::mutex m; std::condition_variable cv; bool open = false, abort = false; } lgate;
    auto wait_listening = [&lgate]() -> bool {
        std::unique_lock<std::mutex> lk(lgate.m);
        lgate.cv.wait(lk, [&] { return lgate.open || lgate.abort; });
        return lgate.open;
    };

    std::vector<std::thread> workers;
    workers.reserve(impl_->workers);
    for (int i = 0; i < impl_->workers; ++i) {
      try {
#ifdef __linux__
        workers.emplace_back([this, shared_port, i, &gate_arrive_and_wait, &wait_listening]() {
#else
        workers.emplace_back([this, &wait_listening]() {
#endif
            look::set_thread_event_loop(impl_->loop.get());

#ifdef __linux__
            // Per-worker SO_REUSEPORT listen socket
            int worker_fd = -1;
            if (fiber_burst) {
                worker_fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (worker_fd >= 0) {
                    int opt = 1;
                    ::setsockopt(worker_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                    ::setsockopt(worker_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
                    sockaddr_in waddr{};
                    waddr.sin_family      = AF_INET;
                    waddr.sin_addr.s_addr = http_bind_addr();
                    waddr.sin_port        = htons((uint16_t)shared_port);
                    // listen() AFTER the startup gate (below) — only bind here.
                    if (::bind(worker_fd, (sockaddr*)&waddr, sizeof(waddr)) < 0) {
                        ::close(worker_fd);
                        worker_fd = -1;
                    } else {
                        int flags = fcntl(worker_fd, F_GETFL, 0);
                        fcntl(worker_fd, F_SETFL, flags | O_NONBLOCK);
                        std::lock_guard<std::mutex> lk(impl_->lfd_mtx);
                        impl_->listen_fds.push_back(worker_fd);
                    }
                }
            }

            // fiber-ok: the thread's scheduler (thread structure)
            static thread_local look::FiberScheduler tl_srv_sched;
            if (fiber_burst && worker_fd >= 0) look::set_thread_scheduler(&tl_srv_sched);
            bool gate_passed = !fiber_burst;   // in fiber mode: wait at the gate once the first accept fiber is set up
            // A worker that could not bind its socket also visits the gate (otherwise the others would wait for it forever).
            if (fiber_burst && worker_fd < 0) { gate_passed = true; gate_arrive_and_wait(); }
            if (!fiber_burst && !wait_listening()) return;
#else
            if (!wait_listening()) return;
#endif

            while (impl_->running) {
#ifdef __linux__
                if (fiber_burst && worker_fd >= 0) {
                    // ── D Fix #2: the acceptor is now A FIBER — the accept fd is multiplexed
                    // in the SAME scheduler epoll as the connection fds (Go
                    // netpoller model). Old model: poll(accept) OUTSIDE +
                    // run_until_complete() blocks until ALL fibers finish →
                    // while keep-alive fibers waited NEW connections were never
                    // accepted (the accept leg of the c=100 hang). Now as soon as the acceptor
                    // yields the scheduler runs the connection fibers;
                    // epoll wakes the acceptor when a new connection arrives.
                    constexpr int FIBER_CAP = 1024;  // per-worker fiber cap
                    FiberScheduler* sched = &tl_srv_sched;
                    auto acceptor = [this, sched](int worker_fd) {
                        auto self = look::Fiber::current()->shared_from_this_fiber();
                        while (impl_->running) {
                            // Backpressure: while at the fiber cap, stop accept,
                            // back off briefly with a pure timer (fd<0 → no epoll registration,
                            // an accept-ready spin is impossible); connections finish meanwhile.
                            if ((int)sched->total_count() > FIBER_CAP) {
                                sched->wait_readable_tmo(self, -1, 5);
                                continue;
                            }
                            sockaddr_storage peer{};
                            socklen_t peer_len = sizeof(peer);
                            int client = accept4(worker_fd,
                                reinterpret_cast<sockaddr*>(&peer), &peer_len, 0);
                            if (client >= 0) {
                                char peer_ip[INET6_ADDRSTRLEN] = {};
                                if (peer.ss_family == AF_INET)
                                    inet_ntop(AF_INET,
                                        &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr,
                                        peer_ip, sizeof(peer_ip));
                                else if (peer.ss_family == AF_INET6)
                                    inet_ntop(AF_INET6,
                                        &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,
                                        peer_ip, sizeof(peer_ip));
                                std::string ip_str = peer_ip;
                                try {
                                    sched->spawn([this, client, ip_str]() {
                                        impl_->handle_connection(client, ip_str);
                                    });
                                } catch (const std::exception& e) {
                                    // The connection's fiber could not be started (memory): close only this
                                    // connection. Previously the exception took down the acceptor, the fd leaked and the client
                                    // was left with no response.
                                    ::close(client);
                                    static std::atomic<unsigned> dropped{0};
                                    unsigned n = ++dropped;
                                    if (n == 1 || n % 1000 == 0)
                                        look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                                            "closed a connection: its fiber could not start (" + std::string(e.what()) +
                                            "); " + std::to_string(n) + " so far. Is the process's virtual memory limited (ulimit -v)?");
                                }
                                continue;   // keep accepting until the queue drains
                            }
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // Queue empty → park the accept fd in the scheduler's epoll.
                                // 200ms ceiling: on shutdown (running=false) we wake up
                                // and exit within 200ms at the latest (previously there was a risk of waiting indefinitely).
                                if (sched->wait_readable_tmo(self, worker_fd, 200) < 0)
                                    break;   // no epoll (should not happen) — leave fiber mode
                                continue;
                            }
                            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                                // Descriptors/memory exhausted: do NOT stop listening, back off briefly (see accept_backoff).
                                sched->wait_readable_tmo(self, -1, 50);
                                continue;
                            }
                            break;   // real accept error
                        }
                    };
                    try {
                    sched->spawn([acceptor, worker_fd]() { acceptor(worker_fd); });
                    } catch (const std::exception& e) {
                        // The acceptor fiber could not be started: connections arriving on this worker's socket
                        // would never be accepted. Like Go: write the reason, exit with an error code (systemd
                        // restarts, the panel/journal shows it). Previously the exception escaped the thread
                        // and std::terminate produced "terminate called without an active exception".
                        std::string msg = "could not start the connection fiber of HTTP worker " +
                            std::to_string(i + 1) + " (" + e.what() + "). Use fewer workers (--workers) "
                            "or raise the process's limits (ulimit -v).";
                        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", msg);
                        std::fprintf(stderr, "Error: %s\n", msg.c_str());
                        std::fflush(nullptr);
                        std::_Exit(1);
                    }
                    // Startup gate: all workers have set up their accept fiber → now listen. If the gate
                    // was cancelled (another thread could not start), exit without listening.
                    if (!gate_passed) {
                        gate_passed = true;
                        if (!gate_arrive_and_wait()) { impl_->running = false; tl_srv_sched.run_until_complete(); break; }
                        if (::listen(worker_fd, 16384) < 0) {
                            std::string msg = "HTTP worker " + std::to_string(i + 1) + " could not listen on port " +
                                std::to_string(shared_port) + " (" + std::strerror(errno) + ")";
                            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", msg);
                            std::fprintf(stderr, "Error: %s\n", msg.c_str());
                            std::fflush(nullptr);
                            std::_Exit(1);
                        }
                        log_listening(shared_port);
                        // Registration with the watcher (below, at the end of run): if this worker gets stuck, the idle
                        // worker it picks will drain its socket. On exit (thread end) the registration is removed before the scheduler is torn down.
                        std::lock_guard<std::mutex> lk(impl_->lfd_mtx);
                        impl_->watched.push_back({sched, worker_fd});
                    }
                    // Run until the acceptor + all connection fibers finish.
                    // The acceptor exits when it sees running=false (≤200ms); the connection
                    // fibers close via idle-timeout → clean wind-down.
                    tl_srv_sched.run_until_complete();
                    continue;   // if running is still true (unusual) set up again
                }
#endif
                // Blocking path (non-fiber or Windows)
                sockaddr_storage peer{};
                socklen_t peer_len = sizeof(peer);
#ifdef __linux__
                int srv_fd = (impl_->server_fd >= 0) ? impl_->server_fd : worker_fd;
                int client = accept4(srv_fd,
                    reinterpret_cast<sockaddr*>(&peer), &peer_len, 0);
#else
                int client = (int)accept((SOCKET)impl_->server_fd,
                    reinterpret_cast<sockaddr*>(&peer), &peer_len);
#endif
                if (client < 0) {
                    if (!impl_->running) break;
                    accept_backoff();
                    continue;
                }
                char peer_ip[INET6_ADDRSTRLEN] = {};
                if (peer.ss_family == AF_INET)
                    inet_ntop(AF_INET,
                        &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr,
                        peer_ip, sizeof(peer_ip));
                else if (peer.ss_family == AF_INET6)
                    inet_ntop(AF_INET6,
                        &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,
                        peer_ip, sizeof(peer_ip));
                impl_->handle_connection(client, peer_ip);
            }

#ifdef __linux__
            if (worker_fd >= 0) {
                look::set_thread_scheduler(nullptr);
                std::lock_guard<std::mutex> lk(impl_->lfd_mtx);
                auto& v = impl_->listen_fds;
                v.erase(std::remove(v.begin(), v.end(), worker_fd), v.end());
                auto& w = impl_->watched;   // the watcher will not touch this scheduler again
                w.erase(std::remove_if(w.begin(), w.end(),
                    [&](const Impl::Watched& x) { return x.fd == worker_fd; }), w.end());
                ::close(worker_fd);
            }
#endif
        });
      } catch (const std::system_error& e) {
        // The worker thread could not be started: stop the ones already started (they see running within ≤200 ms),
        // join, error with a reason — exiting with a joinable thread was a std::terminate.
        impl_->running = false;
        // Release the workers waiting at the gate (they exit without listening) — otherwise join would wait forever.
        { std::lock_guard<std::mutex> lk(gate.m); gate.abort = true; }
        gate.cv.notify_all();
        { std::lock_guard<std::mutex> lk(lgate.m); lgate.abort = true; }
        lgate.cv.notify_all();
        for (auto& t : workers) if (t.joinable()) t.join();
        impl_->loop->stop();
        if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
        throw std::runtime_error("could not start HTTP worker thread " + std::to_string(i + 1) + " of " +
            std::to_string(impl_->workers) + " (" + e.what() + "). Use fewer threads (--workers) or "
            "raise the process's limits (ulimit -u, ulimit -v).");
      }
    }

    // Worker-per-connection model: all workers started → now listen, then release the workers.
    if (!fiber_burst) {
        const int lr = (impl_->server_fd >= 0) ? ::listen(impl_->server_fd, 16384) : -1;
        if (lr < 0) {
            const std::string why = std::strerror(errno);
            impl_->running = false;
            { std::lock_guard<std::mutex> lk(lgate.m); lgate.abort = true; }
            lgate.cv.notify_all();
            for (auto& t : workers) if (t.joinable()) t.join();
            impl_->loop->stop();
            if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
            throw std::runtime_error("could not listen on port " + std::to_string(impl_->port) + " (" + why + ")");
        }
        log_listening(impl_->port);
        { std::lock_guard<std::mutex> lk(lgate.m); lgate.open = true; }
        lgate.cv.notify_all();
    }

#ifdef __linux__
    // Watcher (see Impl::watched): every 2 ms, if there is a connection waiting on the socket of a worker that has been running the same fiber
    // for more than 3 ms (stuck in indivisible work), have the least-loaded worker that is not currently running a fiber
    // drain it. A single thread; workers remove themselves from the registry on exit.
    std::thread monitor;
    if (fiber_burst) {
        try {
            monitor = std::thread([this]() {
                using namespace std::chrono;
                constexpr int64_t kStuckNs = 3'000'000;   // the VM loop yields every 1 ms
                while (impl_->running) {
                    std::this_thread::sleep_for(milliseconds(2));
                    const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
                    std::lock_guard<std::mutex> lk(impl_->lfd_mtx);
                    for (auto& w : impl_->watched) {
                        const int64_t since = w.sched->running_since_ns();
                        if (!since || now - since < kStuckNs || w.draining->load(std::memory_order_acquire)) continue;
                        pollfd p{w.fd, POLLIN, 0};
                        if (::poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) continue;   // none waiting
                        Impl::Watched* to = nullptr;
                        size_t best = SIZE_MAX;
                        for (auto& c : impl_->watched) {
                            if (&c == &w || c.sched->running_since_ns() != 0) continue;
                            const size_t n = c.sched->total_count();
                            if (n < best) { best = n; to = &c; }
                        }
                        if (!to) continue;   // all busy: nothing to do
                        w.draining->store(true, std::memory_order_release);
                        const int fd = w.fd;
                        auto flag = w.draining;
                        try {
                            to->sched->spawn([this, fd, flag]() {
                                impl_->drain_listen(fd);
                                flag->store(false, std::memory_order_release);
                            });
                            to->sched->wake();
                        } catch (...) { flag->store(false, std::memory_order_release); }
                    }
                }
            });
        } catch (const std::system_error&) {}   // could not start: no watcher (old behavior)
    }
#endif

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
#ifdef __linux__
    if (monitor.joinable()) monitor.join();
#endif

    impl_->loop->stop();
    if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
}

void HttpServer::stop() {
    impl_->running = false;
    ::close(impl_->server_fd);
    impl_->server_fd = -1;
    impl_->loop->stop();
}

void HttpServer::begin_shutdown() {
    {
        // draining is under the lock: so it does not race with idle_enter (the registration either happens before the close and
        // is closed below, or happens after and the registration is rejected).
        std::lock_guard<std::mutex> lk(impl_->idle_mtx);
        impl_->draining.store(true, std::memory_order_release);
        impl_->running = false;
#ifndef _WIN32
        // Connections waiting idle: the read side is closed → recv 0 → the connection closes.
        for (int fd : impl_->idle_fds) ::shutdown(fd, SHUT_RD);
#endif
    }
#ifndef _WIN32
    // Listening sockets: the port is released IMMEDIATELY (Go Shutdown closes listeners). If the port stayed bound
    // while shutdown was in progress, a new process started on the same port (restart, deploy) could not bind (measured).
    // The fd itself is not closed — another thread may still be using it and if the freed
    // number were given to a new connection, accept would happen on it. Instead: shutdown (the thread waiting in accept
    // wakes), then a non-listening empty socket is dup2'd onto the same number — the old socket's last
    // reference drops, the port is freed; the number stays valid until shutdown ends (accept returns EINVAL).
    auto release = [](int fd) {
        ::shutdown(fd, SHUT_RDWR);
        int ph = ::socket(AF_INET, SOCK_STREAM, 0);
        if (ph >= 0) { ::dup2(ph, fd); ::close(ph); }
    };
    if (impl_->server_fd >= 0) release(impl_->server_fd);   // per-connection worker model: the shared socket
    std::lock_guard<std::mutex> lk(impl_->lfd_mtx);          // fiber mode: per-worker sockets
    for (int fd : impl_->listen_fds) release(fd);
#endif
}

} // namespace look

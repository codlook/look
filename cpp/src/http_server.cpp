#include "look/http_server.h"
#include "look/websocket.h"
#include "look/sse.h"

#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>

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

namespace look {

// ── HTTP/1.1 parser ───────────────────────────────────────────────────────────

static bool parse_request(const std::string& raw, HttpRequest& req) {
    size_t pos = 0;

    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string req_line = raw.substr(0, line_end);
    size_t s1 = req_line.find(' ');
    size_t s2 = req_line.rfind(' ');
    if (s1 == std::string::npos || s1 == s2) return false;

    req.method  = req_line.substr(0, s1);
    req.version = req_line.substr(s2 + 1);
    std::string full_path = req_line.substr(s1 + 1, s2 - s1 - 1);

    size_t qpos = full_path.find('?');
    if (qpos != std::string::npos) {
        req.path         = full_path.substr(0, qpos);
        req.query_string = full_path.substr(qpos + 1);
    } else {
        req.path = full_path;
    }

    pos = line_end + 2;

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    while (pos < header_end) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) break;
        std::string line = raw.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
        pos = end + 2;
    }

    // WebSocket upgrade
    auto it_up  = req.headers.find("upgrade");
    auto it_key = req.headers.find("sec-websocket-key");
    if (it_up != req.headers.end()) {
        std::string up = it_up->second;
        std::transform(up.begin(), up.end(), up.begin(), ::tolower);
        if (up.find("websocket") != std::string::npos) {
            req.upgrade_websocket = true;
            if (it_key != req.headers.end()) req.ws_key = it_key->second;
        }
    }

    // SSE detection
    auto it_acc = req.headers.find("accept");
    if (it_acc != req.headers.end()) {
        if (it_acc->second.find("text/event-stream") != std::string::npos)
            req.upgrade_sse = true;
    }

    return true;
}

// ── HTTP response builder ─────────────────────────────────────────────────────

std::string HttpResponse::build() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    for (auto& [k, v] : headers) out << k << ": " << v << "\r\n";
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
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
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
            });
        }
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

struct HttpServer::Impl {
    int  port    = 0;
    int  workers = 0;
    HttpHandler  handler;
    WsHandler    ws_handler;
    SseHandler   sse_handler;

    int  server_fd = -1;
    std::atomic<bool> running{false};

    std::unique_ptr<WorkerPool> pool;

    // Event loop — yalnızca WS/SSE async frame okuma için
    std::unique_ptr<EventLoop> loop;
    std::thread loop_thread;

    // WS state
    std::unordered_map<int, std::shared_ptr<WsConnection>> ws_clients;
    std::unordered_map<int, std::string>                   ws_bufs;
    std::mutex ws_mtx;

    // SSE state
    std::unordered_map<int, std::shared_ptr<SseConnection>> sse_clients;
    std::mutex sse_mtx;

    // ── Blocking HTTP bağlantı yöneticisi — worker thread'de çalışır ──────────
    //
    // Her bağlantı için:
    //   1. blocking recv() → headers tamamlanana dek oku
    //   2. parse → WS/SSE upgrade tespiti
    //   3. HTTP: handler → send() → keep-alive döngüsü
    //   4. WS/SSE: event loop'a devret, worker serbest kalır
    //
    // Event loop yok — Apache ile aynı model.
    void handle_connection(int fd) {
        // Boşta timeout — istemci 30sn yanıt vermezse kapat
        timeval tv{30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#ifndef _WIN32
        // Nagle kapalı — küçük paketler hemen gönderilir
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
#endif

        char tmp[65536];

        while (true) {
            // Headers gelene kadar blocking oku
            std::string buf;
            while (buf.find("\r\n\r\n") == std::string::npos) {
                ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
                if (r <= 0) { ::close(fd); return; }
                buf.append(tmp, (size_t)r);
                if (buf.size() > 2 * 1024 * 1024) { ::close(fd); return; }
            }

            HttpRequest req;
            if (!parse_request(buf, req)) { ::close(fd); return; }

            // Body oku (varsa)
            size_t header_end  = buf.find("\r\n\r\n") + 4;
            size_t body_in_buf = buf.size() - header_end;
            auto clen_it = req.headers.find("content-length");
            if (clen_it != req.headers.end()) {
                size_t content_len = std::stoul(clen_it->second);
                if (body_in_buf > 0)
                    req.body = buf.substr(header_end,
                                          std::min(body_in_buf, content_len));
                while (req.body.size() < content_len) {
                    ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
                    if (r <= 0) { ::close(fd); return; }
                    req.body.append(tmp, (size_t)r);
                }
            }

            // WS upgrade → event loop'a devret, worker serbest kalır
            if (req.upgrade_websocket && ws_handler) {
                handle_ws_upgrade(fd, req);
                return;
            }

            // SSE upgrade → event loop'a devret
            if (req.upgrade_sse && sse_handler) {
                handle_sse_upgrade(fd, req);
                return;
            }

            // Keep-alive tespiti (HTTP/1.1 default = keep-alive)
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

            // Dispatch — worker thread doğrudan işler
            HttpResponse resp;
            resp.keep_alive = req_keep_alive;
            resp.headers["Content-Type"] = "application/json; charset=utf-8";
            try {
                handler(req, resp);
            } catch (...) {
                resp.status_code = 500;
                resp.status_text = "Internal Server Error";
                resp.body        = "{\"ok\":false,\"hata\":\"Sunucu hatası\"}";
                resp.keep_alive  = false;
            }

            std::string raw = resp.build();
#ifdef _WIN32
            ssize_t w = ::send(fd, raw.data(), (int)raw.size(), 0);
#else
            ssize_t w = ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
#endif
            if (w <= 0 || !resp.keep_alive) { ::close(fd); return; }
            // keep-alive: sonraki isteği bekle
        }
    }

    // ── WebSocket upgrade — worker thread'de çalışır ──────────────────────────
    void handle_ws_upgrade(int fd, const HttpRequest& req) {
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

        ws_handler(conn, req);

        // fd'yi event loop'a devret: blocking → non-blocking + EPOLL_CTL_ADD
        loop->add_client(fd, [this, fd](const char* d, size_t l) {
            on_ws_data(fd, d, l);
        });
    }

    // ── SSE upgrade — worker thread'de çalışır ───────────────────────────────
    void handle_sse_upgrade(int fd, const HttpRequest& req) {
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
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            sse_clients[fd] = conn;
        }
        look::g_sse_registry.add(conn);

        sse_handler(conn, req);

        // fd'yi event loop'a devret — disconnect tespiti için
        loop->add_client(fd, [this, fd](const char* d, size_t l) {
            on_sse_data(fd, d, l);
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
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            auto it = sse_clients.find(fd);
            if (it != sse_clients.end()) { conn = it->second; sse_clients.erase(it); }
        }
        look::g_sse_registry.remove(fd);
        if (conn && !conn->closed.exchange(true)) {
            if (conn->on_close_cb) {
                auto cb = conn->on_close_cb;
                pool->submit([cb]() { cb(); });
            }
        }
        loop->close_fd(fd);
    }

    // ── WS frame I/O ─────────────────────────────────────────────────────────
    void on_ws_data(int fd, const char* data, size_t len) {
        std::shared_ptr<WsConnection> conn;
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_clients.find(fd);
            if (it == ws_clients.end()) return;
            conn = it->second;
            ws_bufs[fd].append(data, len);
        }
        if (!conn || conn->closed.load()) { close_ws(fd); return; }

        while (true) {
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(ws_mtx);
                auto it = ws_bufs.find(fd);
                if (it == ws_bufs.end()) break;
                snap = it->second;
            }
            WsFrame frame = ws_try_decode_frame(snap);
            if (!frame.complete) break;

            {
                std::lock_guard<std::mutex> lk(ws_mtx);
                auto it = ws_bufs.find(fd);
                if (it != ws_bufs.end()) it->second.erase(0, frame.consumed);
            }

            if (frame.opcode == 0x01 || frame.opcode == 0x02) {
                if (conn->on_message) {
                    auto cb  = conn->on_message;
                    auto msg = frame.payload;
                    pool->submit([cb, msg]() { cb(msg); });
                }
            } else if (frame.opcode == 0x09) {
                std::string pong = ws_encode_pong_frame(frame.payload);
                std::lock_guard<std::mutex> lk(conn->write_mutex);
                conn->send_raw(pong);
            } else if (frame.opcode == 0x08) {
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                if (conn->on_close) {
                    auto cb = conn->on_close;
                    pool->submit([cb]() { cb(); });
                }
                close_ws(fd);
                return;
            }
        }

        if (!conn->closed.load()) {
            loop->async_read(fd, [this, fd](const char* d, size_t l) {
                on_ws_data(fd, d, l);
            });
        } else {
            close_ws(fd);
        }
    }

    void close_ws(int fd) {
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            ws_clients.erase(fd);
            ws_bufs.erase(fd);
        }
        look::g_ws_registry.remove(fd);
        loop->close_fd(fd);
    }
};

// ── HttpServer public API ─────────────────────────────────────────────────────

HttpServer::HttpServer(int port, int workers, HttpHandler handler,
                       WsHandler ws_handler, SseHandler sse_handler)
    : impl_(std::make_unique<Impl>())
{
    impl_->port        = port;
    impl_->workers     = workers;
    impl_->handler     = std::move(handler);
    impl_->ws_handler  = std::move(ws_handler);
    impl_->sse_handler = std::move(sse_handler);
    impl_->loop        = EventLoop::create();
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
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed on port " + std::to_string(port));
    if (::listen(srv, 1024) < 0)
        throw std::runtime_error("listen() failed");

    impl_->server_fd = srv;
}

HttpServer::~HttpServer() {
    impl_->running = false;
    if (impl_->server_fd >= 0) ::close(impl_->server_fd);
}

void HttpServer::run() {
    impl_->running = true;

    // Event loop — arka planda, sadece WS/SSE için
    impl_->loop_thread = std::thread([this]() {
        impl_->loop->run();
    });

    // Her worker thread doğrudan accept() çağırır — Apache/nginx modeli.
    // Queue yok, mutex yok, cv wakeup yok:
    //   eski: accept → mutex lock → queue push → cv notify → worker wakeup → recv
    //   yeni: accept → recv (aynı thread, sıfır overhead)
    std::vector<std::thread> workers;
    workers.reserve(impl_->workers);
    for (int i = 0; i < impl_->workers; ++i) {
        workers.emplace_back([this]() {
            while (impl_->running) {
#ifdef __linux__
                int client = accept4(impl_->server_fd, nullptr, nullptr, 0);
#else
                int client = (int)accept((SOCKET)impl_->server_fd, nullptr, nullptr);
#endif
                if (client < 0) {
                    if (!impl_->running) break;
                    continue;
                }
                impl_->handle_connection(client);
            }
        });
    }

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    impl_->loop->stop();
    if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
}

void HttpServer::stop() {
    impl_->running = false;
    ::close(impl_->server_fd);
    impl_->server_fd = -1;
    impl_->loop->stop();
}

} // namespace look

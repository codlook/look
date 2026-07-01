#include "look/smtp_server.h"
#include "look/event_loop.h"
#include "look/logger.h"

#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/socket.h>
#  include <netinet/in.h>
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
#endif

namespace look {

namespace fs = std::filesystem;

// ── DoS limits (env-configurable) ────────────────────────────────────────────

static int smtp_max_conn() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_CONN");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 1000;
    }();
    return v;
}
static size_t smtp_max_msg() {
    static size_t v = []() -> size_t {
        const char* e = std::getenv("LOOK_SMTP_MAX_MSG_SIZE");
        if (e && *e) { long x = std::atol(e); if (x > 0) return (size_t)x; }
        return 25ul * 1024 * 1024; // 25 MB
    }();
    return v;
}
static int smtp_max_rcpt() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_RCPT");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 100;
    }();
    return v;
}
static int smtp_max_errors() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_ERRORS");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 5;
    }();
    return v;
}
static std::string smtp_banner() {
    static std::string v = []() -> std::string {
        const char* e = std::getenv("LOOK_SMTP_BANNER");
        return (e && *e) ? e : "localhost";
    }();
    return v;
}

// ── SMTP session state machine ────────────────────────────────────────────────

enum class SmtpState {
    GREETING,       // connection opened, send 220
    EHLO,           // waiting for EHLO/HELO
    READY,          // post-EHLO, waiting for AUTH/MAIL/QUIT/RSET
    MAIL_FROM,      // received MAIL FROM, waiting for RCPT TO
    RCPT_TO,        // received ≥1 RCPT TO, waiting for more or DATA
    DATA_INIT,      // received DATA, sending 354
    DATA_BODY,      // accumulating message body until lone "."
    QUIT,           // QUIT received, closing
};

struct SmtpSession {
    int         fd       = -1;
    SmtpState   state    = SmtpState::GREETING;
    std::string buf;           // raw read buffer (may span multiple reads)
    SmtpMessage msg;           // in-progress message
    std::string ehlo_domain;   // client EHLO domain
    int         error_count = 0;
    bool        submission  = false; // connected on :587

    // Reset envelope for RSET / after successful delivery
    void reset_envelope() {
        msg.mail_from.clear();
        msg.rcpt_to.clear();
        msg.data.clear();
        state = SmtpState::READY;
    }
};

// ── Address extraction ─────────────────────────────────────────────────────────
// Extract <addr> from "MAIL FROM:<addr>" or "RCPT TO:<addr>".
static std::string extract_addr(const std::string& line) {
    size_t lt = line.find('<');
    size_t gt = line.find('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt)
        return line.substr(lt + 1, gt - lt - 1);
    // Bare address without angle brackets
    size_t sp = line.find(' ');
    if (sp != std::string::npos) return line.substr(sp + 1);
    return "";
}

// ── SMTP command tokenizer ────────────────────────────────────────────────────
static std::string smtp_verb(const std::string& line) {
    std::string v;
    for (char c : line) {
        if (c == ' ' || c == '\r' || c == '\n') break;
        v += (char)std::toupper((unsigned char)c);
    }
    return v;
}

// ── Worker thread pool ────────────────────────────────────────────────────────

struct WorkItem {
    SmtpMessage msg;
    int         fd;
    SmtpHandler handler;
    EventLoop*  loop;
    std::shared_ptr<SmtpSession> session;
};

struct WorkerPool {
    std::vector<std::thread>      threads;
    std::queue<WorkItem>          queue;
    std::mutex                    mtx;
    std::condition_variable       cv;
    std::atomic<bool>             stop{false};

    void start(int n) {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([this]{ worker_loop(); });
    }
    void shutdown() {
        stop = true;
        cv.notify_all();
        for (auto& t : threads) if (t.joinable()) t.join();
    }
    void enqueue(WorkItem item) {
        std::lock_guard<std::mutex> lk(mtx);
        queue.push(std::move(item));
        cv.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            WorkItem item;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return stop || !queue.empty(); });
                if (stop && queue.empty()) return;
                item = std::move(queue.front());
                queue.pop();
            }
            bool ok = false;
            try { ok = item.handler(item.msg); }
            catch (const std::exception& e) {
                Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                    std::string("handler exception: ") + e.what());
            }
            // Reply on event loop thread
            std::string reply = ok
                ? "250 2.6.0 Message accepted\r\n"
                : "550 5.7.1 Message rejected by policy\r\n";
            item.loop->post([loop = item.loop, fd = item.fd, reply, session = item.session]() mutable {
                loop->async_write(fd, reply, [loop, fd, session](bool) {
                    session->reset_envelope();
                });
            });
        }
    }
};

// ── SmtpServer::Impl ──────────────────────────────────────────────────────────

struct SmtpServer::Impl {
    std::unique_ptr<EventLoop> loop;
    WorkerPool                 pool;
    SmtpHandler                handler;
    std::atomic<int>           conn_count{0};

    // Per-connection session map (fd → session), guarded by loop thread
    std::unordered_map<int, std::shared_ptr<SmtpSession>> sessions;

    // ── Server socket helpers ────────────────────────────────────────────────
    static int make_server_fd(int port) {
#if defined(_WIN32)
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
#else
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
#endif
        if (fd < 0) return -1;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        // Dual-stack: accept both IPv4 and IPv6
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no));

        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons((uint16_t)port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd); return -1;
        }
        if (::listen(fd, 128) < 0) { close(fd); return -1; }
        return fd;
    }

    // ── Session I/O ──────────────────────────────────────────────────────────
    void on_accept(int fd, bool submission) {
        if (conn_count.load() >= smtp_max_conn()) {
            // Too busy — immediate 421 and close
            const char* busy = "421 4.3.2 Service temporarily unavailable\r\n";
#if defined(_WIN32)
            send(fd, busy, (int)strlen(busy), 0);
#else
            ::send(fd, busy, strlen(busy), MSG_NOSIGNAL);
#endif
            close(fd); return;
        }
        conn_count.fetch_add(1);

        auto sess = std::make_shared<SmtpSession>();
        sess->fd         = fd;
        sess->state      = SmtpState::GREETING;
        sess->submission = submission;
        sessions[fd]     = sess;

        std::string banner = "220 " + smtp_banner() + " ESMTP LOOK\r\n";
        loop->async_write(fd, banner, [this, fd, sess](bool ok) {
            if (!ok) { close_session(fd); return; }
            sess->state = SmtpState::EHLO;
            loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
                on_data(fd, sess, data, len);
            });
        });
    }

    void close_session(int fd) {
        sessions.erase(fd);
        loop->close_fd(fd);
        conn_count.fetch_sub(1);
    }

    void send_reply(int fd, std::shared_ptr<SmtpSession> sess,
                    const std::string& reply,
                    std::function<void()> then = nullptr) {
        loop->async_write(fd, reply, [this, fd, sess, then](bool ok) {
            if (!ok) { close_session(fd); return; }
            if (then) then();
            else {
                // Keep reading
                loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
                    on_data(fd, sess, data, len);
                });
            }
        });
    }

    void on_data(int fd, std::shared_ptr<SmtpSession> sess,
                 const char* data, size_t len) {
        if (len == 0) { close_session(fd); return; }
        sess->buf.append(data, len);

        // Process all complete lines from buf
        while (true) {
            size_t nl = sess->buf.find('\n');
            if (nl == std::string::npos) break;

            std::string line = sess->buf.substr(0, nl);
            sess->buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            bool keep_reading = dispatch_line(fd, sess, line);
            if (!keep_reading) return; // session handed off or closed
        }

        // Need more data
        loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
            on_data(fd, sess, data, len);
        });
    }

    // Returns false when reading is handed off to async_write or session is closed
    bool dispatch_line(int fd, std::shared_ptr<SmtpSession> sess,
                       const std::string& line) {
        if (sess->state == SmtpState::DATA_BODY) {
            return handle_data_body(fd, sess, line);
        }

        std::string verb = smtp_verb(line);

        // QUIT — always valid
        if (verb == "QUIT") {
            send_reply(fd, sess, "221 2.0.0 Bye\r\n", [this, fd]{ close_session(fd); });
            return false;
        }

        // RSET — always valid post-EHLO
        if (verb == "RSET") {
            sess->reset_envelope();
            send_reply(fd, sess, "250 2.0.0 Reset\r\n");
            return false;
        }

        // NOOP
        if (verb == "NOOP") {
            send_reply(fd, sess, "250 2.0.0 OK\r\n");
            return false;
        }

        switch (sess->state) {
            case SmtpState::EHLO:
                return handle_ehlo(fd, sess, verb, line);
            case SmtpState::READY:
                return handle_ready(fd, sess, verb, line);
            case SmtpState::MAIL_FROM:
                return handle_mail_from_state(fd, sess, verb, line);
            case SmtpState::RCPT_TO:
                return handle_rcpt_to_state(fd, sess, verb, line);
            case SmtpState::DATA_INIT:
                return handle_data_init(fd, sess, verb, line);
            default:
                return error_reply(fd, sess, "503 5.5.1 Bad sequence\r\n");
        }
    }

    bool handle_ehlo(int fd, std::shared_ptr<SmtpSession> sess,
                     const std::string& verb, const std::string& line) {
        if (verb != "EHLO" && verb != "HELO") {
            return error_reply(fd, sess, "503 5.5.1 Send EHLO first\r\n");
        }
        size_t sp = line.find(' ');
        sess->ehlo_domain = (sp != std::string::npos) ? line.substr(sp + 1) : "unknown";

        std::string r = "250-" + smtp_banner() + "\r\n"
                        "250-SIZE " + std::to_string(smtp_max_msg()) + "\r\n"
                        "250-8BITMIME\r\n"
                        "250-PIPELINING\r\n"
                        "250 SMTPUTF8\r\n";
        sess->state = SmtpState::READY;
        send_reply(fd, sess, r);
        return false;
    }

    bool handle_ready(int fd, std::shared_ptr<SmtpSession> sess,
                      const std::string& verb, const std::string& line) {
        if (verb == "MAIL") {
            std::string addr = extract_addr(line);
            if (addr.empty()) {
                return error_reply(fd, sess, "501 5.1.7 Bad sender address\r\n");
            }
            sess->msg.mail_from = addr;
            sess->state = SmtpState::MAIL_FROM;
            send_reply(fd, sess, "250 2.1.0 OK\r\n");
            return false;
        }
        if (verb == "EHLO" || verb == "HELO") {
            // Re-EHLO resets state
            return handle_ehlo(fd, sess, verb, line);
        }
        return error_reply(fd, sess, "503 5.5.1 Need MAIL command\r\n");
    }

    bool handle_mail_from_state(int fd, std::shared_ptr<SmtpSession> sess,
                                const std::string& verb, const std::string& line) {
        if (verb != "RCPT") {
            return error_reply(fd, sess, "503 5.5.1 Need RCPT TO\r\n");
        }
        std::string addr = extract_addr(line);
        if (addr.empty()) {
            return error_reply(fd, sess, "501 5.1.3 Bad recipient address\r\n");
        }
        if ((int)sess->msg.rcpt_to.size() >= smtp_max_rcpt()) {
            return error_reply(fd, sess, "452 4.5.3 Too many recipients\r\n");
        }
        sess->msg.rcpt_to.push_back(addr);
        sess->state = SmtpState::RCPT_TO;
        send_reply(fd, sess, "250 2.1.5 OK\r\n");
        return false;
    }

    bool handle_rcpt_to_state(int fd, std::shared_ptr<SmtpSession> sess,
                               const std::string& verb, const std::string& line) {
        if (verb == "RCPT") {
            // Another RCPT TO
            std::string addr = extract_addr(line);
            if (addr.empty()) {
                return error_reply(fd, sess, "501 5.1.3 Bad recipient address\r\n");
            }
            if ((int)sess->msg.rcpt_to.size() >= smtp_max_rcpt()) {
                return error_reply(fd, sess, "452 4.5.3 Too many recipients\r\n");
            }
            sess->msg.rcpt_to.push_back(addr);
            send_reply(fd, sess, "250 2.1.5 OK\r\n");
            return false;
        }
        if (verb == "DATA") {
            sess->state = SmtpState::DATA_INIT;
            send_reply(fd, sess, "354 Start mail input; end with <CRLF>.<CRLF>\r\n",
                [this, fd, sess]() {
                    sess->state = SmtpState::DATA_BODY;
                    loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                        on_data(fd, sess, d, l);
                    });
                });
            return false;
        }
        return error_reply(fd, sess, "503 5.5.1 Need DATA or more RCPT\r\n");
    }

    bool handle_data_init(int /*fd*/, std::shared_ptr<SmtpSession> /*sess*/,
                          const std::string& /*verb*/, const std::string& /*line*/) {
        // Should not reach here — DATA_INIT transitions to DATA_BODY via callback
        return true;
    }

    bool handle_data_body(int fd, std::shared_ptr<SmtpSession> sess,
                          const std::string& line) {
        // RFC 5321 §4.5.2 — lone "." terminates message
        if (line == ".") {
            if (sess->msg.data.size() > smtp_max_msg()) {
                send_reply(fd, sess, "552 5.3.4 Message too large\r\n",
                    [this, fd, sess]() {
                        sess->reset_envelope();
                        loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                            on_data(fd, sess, d, l);
                        });
                    });
                return false;
            }
            // Hand off to worker thread
            WorkItem item;
            item.msg     = sess->msg;
            item.fd      = fd;
            item.handler = handler;
            item.loop    = loop.get();
            item.session = sess;
            pool.enqueue(std::move(item));
            return false;
        }
        // RFC 5321 §4.5.2 dot-unstuffing: leading "." removed
        if (!line.empty() && line[0] == '.') {
            sess->msg.data += line.substr(1) + "\r\n";
        } else {
            sess->msg.data += line + "\r\n";
        }
        if (sess->msg.data.size() > smtp_max_msg()) {
            send_reply(fd, sess, "552 5.3.4 Message too large\r\n",
                [this, fd, sess]() {
                    sess->reset_envelope();
                    loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                        on_data(fd, sess, d, l);
                    });
                });
            return false;
        }
        return true; // continue accumulating
    }

    bool error_reply(int fd, std::shared_ptr<SmtpSession> sess,
                     const std::string& reply) {
        sess->error_count++;
        if (sess->error_count >= smtp_max_errors()) {
            send_reply(fd, sess, "421 4.7.0 Too many errors\r\n",
                [this, fd]{ close_session(fd); });
            return false;
        }
        send_reply(fd, sess, reply);
        return false;
    }
};

// ── SmtpServer public API ─────────────────────────────────────────────────────

SmtpServer::SmtpServer(int port_smtp, int port_sub, int port_smtps,
                       int workers, SmtpHandler handler)
    : impl_(std::make_unique<Impl>())
{
    impl_->loop    = EventLoop::create();
    impl_->handler = std::move(handler);
    impl_->pool.start(workers);

    auto bind_port = [&](int port, bool submission) {
        if (port <= 0) return;
        int fd = Impl::make_server_fd(port);
        if (fd < 0) {
            Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                "cannot bind port " + std::to_string(port));
            return;
        }
        bool sub = submission;
        impl_->loop->listen(fd, [this, sub](int client_fd) {
            impl_->on_accept(client_fd, sub);
        });
        Logger::instance().log(LogLevel::LOG_INFO, "smtp",
            "listening on :" + std::to_string(port));
    };

    bind_port(port_smtp,  false); // :25  MTA-to-MTA
    bind_port(port_sub,   true);  // :587 authenticated submission
    // :465 implicit TLS — placeholder, wired when TLS layer is added
    (void)port_smtps;
}

SmtpServer::~SmtpServer() {
    impl_->pool.shutdown();
}

void SmtpServer::run()  { impl_->loop->run();  }
void SmtpServer::stop() { impl_->loop->stop(); }

// ── Maildir delivery ──────────────────────────────────────────────────────────
// Maildir format: base_dir/<mailbox>/{new,cur,tmp}/
// Each message = tmp/<unique> → rename to new/<unique> (atomic)

std::string deliver_maildir(const std::string& base_dir,
                            const std::string& mailbox,
                            const SmtpMessage& msg) {
    fs::path mbox = fs::path(base_dir) / mailbox;
    fs::create_directories(mbox / "new");
    fs::create_directories(mbox / "cur");
    fs::create_directories(mbox / "tmp");

    // Unique filename: timestamp.pid.random:2, (Maildir standard)
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                       now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << ts << "." << std::hex << dist(rng);
    std::string unique = oss.str();

    fs::path tmp_path = mbox / "tmp" / unique;
    fs::path new_path = mbox / "new" / unique;

    {
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f) throw std::runtime_error("maildir: cannot write to " + tmp_path.string());

        // Prepend Return-Path and Received headers
        f << "Return-Path: <" << msg.mail_from << ">\r\n";
        f << "Received: from " << msg.remote_ip << "\r\n";
        f << msg.data;
    }

    fs::rename(tmp_path, new_path); // atomic on POSIX
    return new_path.filename().string();
}

} // namespace look

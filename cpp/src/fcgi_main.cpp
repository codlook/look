// look-fcgi.exe — LOOK FastCGI runtime
//
// Two modes:
//
//   1. stdin/stdout (mod_fcgid — Windows/XAMPP):
//      look-fcgi.exe
//      Apache httpd.conf:
//        FcgidWrapper "C:/xampp/cgi-bin/look-fcgi.exe" .lk
//        AddHandler fcgid-script .lk
//
//   2. TCP (mod_proxy_fcgi — PHP-FPM model, works everywhere):
//      look-fcgi.exe --port 9000
//      Apache httpd.conf:
//        ProxyPassMatch "^/(.*\.lk.*)$" "fcgi://127.0.0.1:9000/$1"
//      Nginx:
//        fastcgi_pass 127.0.0.1:9000;
//
// Developer code never changes — only the deployment model changes.

#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normally CMake compile-def provides it (single source)
#endif
#include <ctime>
#include "look/fcgi.h"
#include "look/shutdown.h"     // graceful shutdown (SIGTERM/SIGINT)
#include "look/weak_settings.h" // weakening-setting warnings at startup
#include "look/mem_budget.h"   // memory budget (LOOK_MEMORY_LIMIT)
#include "look/health_path.h"  // built-in health path — single source (HTTP mode uses it too)
#include "look/fcgi_parse.h"   // pure wire-parse seam (no drift — table/fuzz test target)
#include "look/host_port.h"    // --port: Go flag model (digits only, 1-65535)
#include "look/path_guard.h"   // canonical symlink/traversal gate (a string check is not enough)
#include "look/http_client.h"
#include "look/thread_start.h"     // start_pool_threads
#include "look/runtime_init.h"    // runtime_init (CA probe + SQLite init, process start)
#include "look/session_files.h"   // LOOK_SESSION_DIR check at startup
#include "look/lexer.h"
#include "look/parser.h"
#include "look/runtime_host.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/http_server.h"   // http_fiber_dispatch_enabled — default worker count

#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <iterator>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>

#ifndef LOOK_BUILD          // if there is no CMake stamp (raw build), a safe default
#define LOOK_BUILD "src"
#endif

#ifdef _WIN32
  #include <direct.h>
#else
  #include <unistd.h>
#endif

// ── Thread pool ───────────────────────────────────────────────────────────────

class ThreadPool {
public:
    explicit ThreadPool(int n) : stop_(false) {
        look::start_pool_threads(workers_, n, "FastCGI worker", [this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                        if (stop_ && queue_.empty()) return;
                        task = std::move(queue_.front());
                        queue_.pop();
                    }
                    task();
                }
            },
            [this] { { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; } cv_.notify_all(); });
    }

    void submit(std::function<void()> f) {
        { std::lock_guard<std::mutex> lk(mtx_); queue_.push(std::move(f)); }
        cv_.notify_one();
    }

    ~ThreadPool() {
        { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  queue_;
    std::mutex                         mtx_;
    std::condition_variable            cv_;
    bool                               stop_;
};

namespace fs = std::filesystem;

// ── FcgiConn implementation ───────────────────────────────────────────────────

namespace look {

FcgiConn::FcgiConn() : tcp_mode_(false) {
#ifdef _WIN32
    h_in_  = GetStdHandle(STD_INPUT_HANDLE);
    h_out_ = GetStdHandle(STD_OUTPUT_HANDLE);
#endif
}

#ifdef _WIN32
FcgiConn::FcgiConn(SOCKET sock) : tcp_mode_(true), tcp_sock_(sock) {}
#else
FcgiConn::FcgiConn(int sock) : tcp_mode_(true), tcp_sock_(sock) {}
#endif

FcgiConn::~FcgiConn() {
    if (tcp_mode_) {
#ifdef _WIN32
        if (tcp_sock_ != INVALID_SOCKET) closesocket(tcp_sock_);
#else
        if (tcp_sock_ >= 0) ::close(tcp_sock_);
#endif
    }
}

FcgiConn::FcgiConn(FcgiConn&& o) noexcept
    : tcp_mode_(o.tcp_mode_)
#ifdef _WIN32
    , h_in_(o.h_in_), h_out_(o.h_out_), tcp_sock_(o.tcp_sock_)
#else
    , fd_in_(o.fd_in_), fd_out_(o.fd_out_), tcp_sock_(o.tcp_sock_)
#endif
{
#ifdef _WIN32
    o.tcp_sock_ = INVALID_SOCKET;
#else
    o.tcp_sock_ = -1;
#endif
}

FcgiConn& FcgiConn::operator=(FcgiConn&& o) noexcept {
    if (this != &o) {
        this->~FcgiConn();
        new (this) FcgiConn(std::move(o));
    }
    return *this;
}

void FcgiConn::set_read_timeout(int seconds) {
    if (!tcp_mode_) return;
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(tcp_sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(tcp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool FcgiConn::raw_read(uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        if (tcp_mode_) {
#ifdef _WIN32
            int n = recv(tcp_sock_, (char*)(buf + done), (int)(len - done), 0);
            if (n <= 0) return false;
#else
            ssize_t n = ::recv(tcp_sock_, buf + done, len - done, 0);
            if (n <= 0) return false;
#endif
            done += (size_t)n;
        } else {
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(h_in_, buf + done, (DWORD)(len - done), &n, NULL) || n == 0)
                return false;
            done += n;
#else
            ssize_t n = ::read(fd_in_, buf + done, len - done);
            if (n <= 0) return false;
            done += (size_t)n;
#endif
        }
    }
    return true;
}

bool FcgiConn::raw_write(const uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        if (tcp_mode_) {
#ifdef _WIN32
            int n = send(tcp_sock_, (const char*)(buf + done), (int)(len - done), 0);
            if (n <= 0) return false;
#else
            ssize_t n = ::send(tcp_sock_, buf + done, len - done, 0);
            if (n <= 0) return false;
#endif
            done += (size_t)n;
        } else {
#ifdef _WIN32
            DWORD n = 0;
            if (!WriteFile(h_out_, buf + done, (DWORD)(len - done), &n, NULL) || n == 0)
                return false;
            done += n;
#else
            ssize_t n = ::write(fd_out_, buf + done, len - done);
            if (n <= 0) return false;
            done += (size_t)n;
#endif
        }
    }
    return true;
}

bool FcgiConn::read_record(uint8_t& type, uint16_t& req_id,
                             std::vector<uint8_t>& content) {
    uint8_t hdr[8];
    if (!raw_read(hdr, 8)) return false;
    FcgiRecordHeader h = fcgi_decode_header(hdr);  // pure decoding (fcgi_parse.h)
    type   = h.type;
    req_id = h.req_id;

    content.resize(h.content_len);
    if (h.content_len > 0 && !raw_read(content.data(), h.content_len)) return false;
    if (h.padding_len > 0) {
        uint8_t pad[255];
        if (!raw_read(pad, h.padding_len)) return false;
    }
    return true;
}

void FcgiConn::write_record(uint8_t type, uint16_t req_id,
                              const uint8_t* data, size_t len) {
    size_t offset = 0;
    do {
        size_t  chunk   = (len - offset > 65535) ? 65535 : (len - offset);
        uint8_t padding = (uint8_t)((8 - (chunk % 8)) % 8);

        uint8_t hdr[8];
        hdr[0] = 1; hdr[1] = type;
        hdr[2] = (uint8_t)(req_id >> 8); hdr[3] = (uint8_t)(req_id & 0xFF);
        hdr[4] = (uint8_t)(chunk  >> 8); hdr[5] = (uint8_t)(chunk  & 0xFF);
        hdr[6] = padding; hdr[7] = 0;

        raw_write(hdr, 8);
        if (chunk   > 0) raw_write(data + offset, chunk);
        if (padding > 0) { uint8_t pad[8] = {}; raw_write(pad, padding); }
        offset += chunk;
    } while (offset < len);
}

uint32_t FcgiConn::read_len(const uint8_t*& p, const uint8_t* end) {
    return fcgi_read_len(p, end);   // pure seam (fcgi_parse.h) — no drift
}

void FcgiConn::parse_params(const uint8_t* p, size_t len,
                              std::map<std::string, std::string>& out) {
    fcgi_parse_params(p, len, out); // pure seam (fcgi_parse.h) — no drift
}

bool FcgiConn::accept(FcgiRequest& req) {
    req = FcgiRequest{};
    bool in_request = false;
    size_t params_bytes = 0;
    while (true) {
        uint8_t type; uint16_t rid;
        std::vector<uint8_t> content;
        if (!read_record(type, rid, content)) return false;

        switch (type) {
            case FCGI_BEGIN_REQUEST:
                if (content.size() >= 8) {
                    req.id        = rid;
                    req.keep_conn = fcgi_begin_keep_conn(content.data(), content.size());
                    in_request    = true;
                    params_bytes  = 0;
                }
                break;
            case FCGI_PARAMS:
                if (in_request && rid == req.id) {
                    // Total PARAMS limit = the header limit in HTTP mode (2 MB). It used to be unbounded:
                    // someone reaching the FCGI port directly could fill memory with endless PARAMS records.
                    params_bytes += content.size();
                    if (params_bytes > 2 * 1024 * 1024) { end_request(rid, 1); return false; }
                    if (!content.empty())
                        parse_params(content.data(), content.size(), req.params);
                }
                break;
            case FCGI_STDIN:
                if (in_request && rid == req.id) {
                    if (content.empty()) return true; // request ready
                    // Defense-in-depth: FCGI body cap. Normally nginx cuts it with
                    // client_max_body_size, but if LOOK is exposed directly to an
                    // FCGI client, an unbounded body = memory DoS.
                    static const size_t max_body = []() -> size_t {
                        const char* e = std::getenv("LOOK_MAX_BODY_SIZE");
                        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
                        return 10 * 1024 * 1024;
                    }();
                    if (req.body.size() + content.size() > max_body) {
                        end_request(rid, 1); return false;  // drop the connection
                    }
                    req.body.append(content.begin(), content.end());
                }
                break;
            case FCGI_ABORT_REQUEST:
                end_request(rid, 1);
                req = FcgiRequest{}; in_request = false; params_bytes = 0;
                break;
            case FCGI_GET_VALUES:
                write_record(FCGI_GET_VALUES_RESULT, 0, nullptr, 0);
                break;
            default: break;
        }
    }
}

void FcgiConn::write_stdout(uint16_t req_id, const std::string& data) {
    if (!data.empty())
        write_record(FCGI_STDOUT, req_id,
                     reinterpret_cast<const uint8_t*>(data.data()), data.size());
    write_record(FCGI_STDOUT, req_id, nullptr, 0);
}

void FcgiConn::write_stderr(uint16_t req_id, const std::string& data) {
    if (!data.empty())
        write_record(FCGI_STDERR, req_id,
                     reinterpret_cast<const uint8_t*>(data.data()), data.size());
    write_record(FCGI_STDERR, req_id, nullptr, 0);
}

void FcgiConn::end_request(uint16_t req_id, uint32_t app_status) {
    uint8_t body[8] = {
        (uint8_t)(app_status >> 24), (uint8_t)(app_status >> 16),
        (uint8_t)(app_status >> 8),  (uint8_t)(app_status & 0xFF),
        FCGI_REQUEST_COMPLETE, 0, 0, 0
    };
    write_record(FCGI_END_REQUEST, req_id, body, 8);
}

// ── FcgiServer (TCP listener) ─────────────────────────────────────────────────

FcgiServer::FcgiServer(int port) : port_(port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
#endif

    int opt = 1;
#ifdef _WIN32
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    // The FastCGI port is ONLY for the local web server (Apache/nginx on 127.0.0.1). Binding to 0.0.0.0
    // exposes the raw FastCGI protocol to the network → front-end filtering/auth is bypassed and
    // FastCGI parameter injection (SCRIPT_FILENAME etc.) becomes possible remotely. Default is
    // loopback; for a multi-host proxy setup, explicit opt-in: LOOK_FCGI_BIND.
    //   (empty/unset)   → 127.0.0.1 (safe default)
    //   "0.0.0.0"/"any"  → all interfaces (deliberate exposure)
    //   "1.2.3.4"        → a specific address
    const char* bind_env = std::getenv("LOOK_FCGI_BIND");
    if (bind_env && (std::string(bind_env) == "0.0.0.0" || std::string(bind_env) == "any")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (bind_env && *bind_env) {
        if (inet_pton(AF_INET, bind_env, &addr.sin_addr) != 1)
            throw std::runtime_error("look-fcgi: invalid LOOK_FCGI_BIND address: " + std::string(bind_env));
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 — safe default
    }
    addr.sin_port        = htons((uint16_t)port_);

#ifdef _WIN32
    if (bind(listen_sock_, (SOCKADDR*)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock_, SOMAXCONN) != 0)
        throw std::runtime_error("look-fcgi: cannot bind TCP port " + std::to_string(port_));
#else
    if (::bind(listen_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        ::listen(listen_sock_, SOMAXCONN) != 0)
        throw std::runtime_error("look-fcgi: cannot bind TCP port " + std::to_string(port_));
#endif
}

FcgiServer::~FcgiServer() {
#ifdef _WIN32
    if (listen_sock_ != INVALID_SOCKET) { closesocket(listen_sock_); WSACleanup(); }
#else
    if (listen_sock_ >= 0) ::close(listen_sock_);
#endif
}

void FcgiServer::stop() {
    // wakes up the run() loop waiting in accept (the socket is not closed: the destructor closes it).
#ifdef _WIN32
    if (listen_sock_ != INVALID_SOCKET) ::shutdown(listen_sock_, SD_BOTH);
#else
    if (listen_sock_ >= 0) ::shutdown(listen_sock_, SHUT_RDWR);
#endif
}

void FcgiServer::run(Handler handler) {
    while (true) {
#ifdef _WIN32
        SOCKET client = ::accept(listen_sock_, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        handler(FcgiConn(client));
#else
        int client = ::accept(listen_sock_, nullptr, nullptr);
        if (client < 0) break;
        handler(FcgiConn(client));
#endif
    }
}

} // namespace look

// ── Helper functions ──────────────────────────────────────────────────────────

static std::string json_escape_str(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

static bool is_production() {
    const char* env = std::getenv("APP_ENV");
    const char* dbg = std::getenv("APP_DEBUG");
    return !(env && std::string(env) == "development") &&
           !(dbg && std::string(dbg) == "true");
}

static std::string build_error_response(int status, const std::string& title,
                                         const std::string& msg) {
    std::string body = is_production()
        // "error"/"code" = English keys; "hata"/"kod" stay alongside them throughout 1.x.
        ? "{\"ok\":false,\"error\":\"Server error.\",\"code\":" + std::to_string(status)
          + ",\"hata\":\"Server error.\",\"kod\":" + std::to_string(status) + "}\n"
        : "{\"ok\":false,\"error\":\"" + json_escape_str(msg) + "\",\"code\":" + std::to_string(status)
          + ",\"hata\":\"" + json_escape_str(msg) + "\",\"kod\":" + std::to_string(status) + "}\n";
    return "Status: " + std::to_string(status) + " " + title + "\r\n"
           "Content-Type: application/json; charset=utf-8\r\n"
           "Access-Control-Allow-Origin: *\r\n\r\n" + body;
}

// Request parameters are NOT WRITTEN to the process environment. It used to write all of every request's parameters (the client's
// headers as HTTP_*) into the process environment with setenv, from 64 threads at the same time:
//   * getenv (env(), TEMP in multipart upload) raced with the concurrent setenv and gave SIGSEGV;
//   * glibc never releases old environment strings: every new header value was permanent memory (+49 MB / 5000
//     requests), and every getenv got slower as the environment grew;
//   * one request's header (X-Secret) was visible in later requests' env("HTTP_X_SECRET").
// The request's information is read with request::; env() is configuration (.env + process environment).

static std::string resolve_script(const std::map<std::string, std::string>& params) {
    auto get = [&](const std::string& k) {
        auto it = params.find(k); return it != params.end() ? it->second : "";
    };
    // Path traversal rejection — the resolved path derives from client-influenced CGI variables
    // (SCRIPT_NAME/PATH_INFO/PATH_TRANSLATED); LOOK executes this .lk file as CODE.
    // A ".." segment escaping the doc root could allow arbitrary .lk execution
    // (RCE). Reject a candidate containing traversal.
    auto no_traversal = [](const std::string& s) {
        return s.find("../") == std::string::npos &&
               s.find("..\\") == std::string::npos &&
               !(s.size() >= 2 && s.substr(s.size() - 2) == "..");
    };
    auto is_real_lk = [&](const std::string& s) {
        // exclude fake paths like proxy:fcgi://...
        if (s.empty()) return false;
        if (s.rfind("proxy:", 0) == 0) return false;
        if (s.rfind("fcgi:",  0) == 0) return false;
        if (!no_traversal(s)) return false;   // ".." traversal → reject
        return s.size() > 3 && s.substr(s.size() - 3) == ".lk";
    };

    // 1. PATH_TRANSLATED — in CGI Action directive mode the real path arrives
    std::string pt = get("PATH_TRANSLATED");
    if (is_real_lk(pt)) return pt;

    // 2. SCRIPT_FILENAME — use it if it is a real Windows path
    std::string sf = get("SCRIPT_FILENAME");
    if (is_real_lk(sf)) return sf;

    // 3. DOCUMENT_ROOT + SCRIPT_NAME — in mod_proxy_fcgi RewriteRule [P] mode
    //    example: SCRIPT_NAME=/index.lk, DOCUMENT_ROOT=C:/xampp/htdocs
    std::string doc = get("DOCUMENT_ROOT");
    std::string sn  = get("SCRIPT_NAME");
    if (!doc.empty() && !sn.empty() && no_traversal(sn) &&
        sn.size() > 3 && sn.substr(sn.size()-3) == ".lk") {
        // CANONICAL GATE: LOOK builds the doc+sn path, Apache does not touch the file (mod_proxy_fcgi
        // [P]) → SymLinksIfOwnerMatch is not applied. The string no_traversal cannot see a symlink;
        // weakly_canonical resolves symlink+".." and rejects a candidate that leaves the doc root (multi-tenant).
        std::string cand = doc + sn;
        if (look::path_under_root(doc, cand)) return cand;
    }

    // 4. Resolve .lk from PATH_INFO (Apache Action + PATH_INFO model)
    std::string pi = get("PATH_INFO");
    if (!doc.empty() && !pi.empty() && no_traversal(pi)) {
        size_t pos = pi.find(".lk");
        if (pos != std::string::npos) {
            std::string cand = doc + pi.substr(0, pos + 3);
            if (look::path_under_root(doc, cand)) return cand;   // canonical gate (symlink)
        }
    }

    // 5. Fallback: DOCUMENT_ROOT/index.lk — all requests arriving via RewriteRule [P]:
    //    REQUEST_URI is passed to WebContext for routing, the script is always index.lk
    if (!doc.empty()) {
        std::string idx = doc;
        // Windows: normalize backslashes
        for (char& c : idx) if (c == '\\') c = '/';
        if (idx.back() != '/') idx += '/';
        idx += "index.lk";
        return idx;
    }

    return "";
}

// Fill WebContext from FCGI params (independent of env vars)
static look::WebContext make_web_context(const look::FcgiRequest& req) {
    auto param = [&](const std::string& k) -> std::string {
        auto it = req.params.find(k);
        return it != req.params.end() ? it->second : "";
    };

    look::WebContext web;

    web.method = param("REQUEST_METHOD");
    if (web.method.empty()) web.method = "GET";
    web.document_root = param("DOCUMENT_ROOT");   // file::store web-root check (used to be via setenv)

    // Path: take from REQUEST_URI
    {
        std::string uri = param("REQUEST_URI");
        size_t q = uri.find('?');
        if (q != std::string::npos) {
            web.query_string = uri.substr(q + 1);
            uri = uri.substr(0, q);
        }
        web.get_params = look::WebContext::parse_query(web.query_string);
        std::string sn = param("SCRIPT_NAME");
        bool sn_is_lk  = sn.size() > 3 && sn.substr(sn.size()-3) == ".lk";
        if (sn_is_lk && uri.rfind(sn, 0) == 0) uri = uri.substr(sn.size());
        web.path = uri.empty() ? "/" : uri;
    }

    web.body         = req.body;
    web.content_type = param("CONTENT_TYPE");
    web.parse_post_body();   // urlencoded + multipart — single source (web.cpp)

    web.remote_addr = param("REMOTE_ADDR");
    // HTTPS from the FCGI params is set by the web server (Apache/nginx), not the client — trusted.
    { std::string h = param("HTTPS"); for (auto& c : h) c = (char)std::tolower((unsigned char)c);
      web.is_https = (h == "on" || h == "1" || param("REQUEST_SCHEME") == "https"); }
    web.host = look::WebContext::clean_host(param("HTTP_HOST"));   // same: the web server's Host
    std::string ck  = param("HTTP_COOKIE");
    if (!ck.empty()) web.cookies_in = look::WebContext::parse_cookies(ck);

    // Populate headers_in from HTTP_* FCGI params (HTTP_AUTHORIZATION → authorization)
    for (auto& [k, v] : req.params) {
        if (k.size() > 5 && k.substr(0, 5) == "HTTP_") {
            std::string name = k.substr(5);
            // HTTP_AUTHORIZATION → authorization, HTTP_X_FORWARDED_FOR → x-forwarded-for
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return c == '_' ? '-' : std::tolower(c); });
            web.headers_in[name] = v;
        }
    }

    return web;
}

// ── Request handling — concurrent dispatch model ──────────────────────────────

// Shared application engine — defined in http_main.cpp
bool look_http_engine_ensure(const std::string& script_path, std::string& err);
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path);

static void handle_request(look::FcgiRequest& req, look::FcgiConn& conn) {
    // Built-in health endpoint — no LOOK context, no script needed
    {
        const std::string& health_path = look::health_path();
        auto uri_it = req.params.find("REQUEST_URI");
        auto method_it = req.params.find("REQUEST_METHOD");
        if (uri_it != req.params.end() && method_it != req.params.end()) {
            std::string uri = uri_it->second;
            auto q = uri.find('?');
            if (q != std::string::npos) uri = uri.substr(0, q);
            if (method_it->second == "GET" && uri == health_path) {
                std::string response =
                    "Status: 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n"
                    "ok";
                conn.write_stdout(req.id, response);
                conn.end_request(req.id, 0);
                return;
            }
        }
    }

    std::string script_path = resolve_script(req.params);
    if (script_path.empty()) {
        conn.write_stdout(req.id, build_error_response(400, "Bad Request", "No script."));
        conn.end_request(req.id, 1);
        return;
    }

    try {
        // ── Shared engine (http_main.cpp) — setup + hot reload + VM ──
        // FCGI mode uses the SAME dispatch engine (bytecode VM) as HTTP mode.
        std::string setup_err;
        if (!look_http_engine_ensure(script_path, setup_err)) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI",
                "Setup error: " + setup_err + " | " + script_path);
            conn.write_stdout(req.id,
                build_error_response(503, "Service Unavailable", setup_err));
            conn.end_request(req.id, 0);
            return;
        }

        look::WebContext web = make_web_context(req);
        std::ostringstream output;
        look_app_dispatch(web, output, web.path);

        std::string body = web.response_body.empty() ? output.str() : web.response_body;
        conn.write_stdout(req.id, web.build_headers() + body);

    } catch (const std::exception& e) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI",
            std::string("Uncaught: ") + e.what() + " | " + script_path);
        conn.write_stderr(req.id, std::string("LOOK FCGI error: ") + e.what());
        conn.write_stdout(req.id, build_error_response(500, "Internal Server Error", e.what()));
    }

    conn.end_request(req.id, 0);
}

// ── main ─────────────────────────────────────────────────────────────────────

// Declared in http_main.cpp
void run_http_mode(int port, int workers, const std::string& script_path);

int main(int argc, char* argv[]) {
#ifndef _WIN32
    // fill the tz global state before the threads start (localtime_r race fix)
    tzset();
#endif
    // Graceful shutdown: SIGTERM/SIGINT are received on a separate thread — BEFORE any thread is opened
    // (runtime_init and setup may open them; the mask is inherited). look/shutdown.h
    look::shutdown::install();
    look::runtime_init();  // all process-start init (CA probe + SQLite one-time init) — shared by the 3 entry points
    // --port NNNN     → TCP mode (fcgi) or http mode
    // --workers N     → number of concurrent dispatch threads (default: CPU cores)
    // --mode fcgi     → FastCGI mode (default)
    // --mode http     → HTTP/1.1 mode, Apache bypass, event loop
    // --bind ADDR     → --mode http listen address (LOOK_BIND; default all interfaces)
    // no arguments    → stdin/stdout mode (mod_fcgid)
    int         tcp_port = 0;
    int         workers  = 0;
    std::string mode     = "fcgi";
    std::string script   = "index.lk";

    // version / --version / -v → print the version and exit
    if (argc >= 2 && (std::strcmp(argv[1], "version") == 0 ||
                      std::strcmp(argv[1], "--version") == 0 ||
                      std::strcmp(argv[1], "-v") == 0)) {
        std::string platform =
#ifdef _WIN32
            "windows";
#elif defined(__linux__)
            "linux";
#elif defined(__APPLE__)
            "darwin";
#else
            "unknown";
#endif
        std::string arch =
#if defined(__x86_64__) || defined(_M_X64)
            "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
            "arm64";
#else
            "unknown";
#endif
        std::cout << "LOOK " LOOK_VERSION " (" << LOOK_BUILD << ", " << platform << "/" << arch << ")\n";
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--port") == 0 || std::strcmp(argv[i], "-p") == 0)
            && i + 1 < argc) {
            // Go flag model: a bad value → error + exit 2. It used to use atoi: "--port 80a" → 80,
            // "--port abc" → 0 → silently 9000 in http mode, silently stdin mode in fcgi mode.
            if (!look::parse_port_number(argv[++i], tcp_port)) {
                std::cerr << "lk-fcgi: invalid --port value \"" << argv[i]
                          << "\" (expected a number from 1 to 65535)\n";
                return 2;
            }
        }
        else if ((std::strcmp(argv[i], "--workers") == 0 || std::strcmp(argv[i], "-w") == 0)
            && i + 1 < argc)
            workers = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc)
            script = argv[++i];
        else if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            // --mode http listen address (same as LOOK_BIND; the command line overrides the environment)
#ifdef _WIN32
            _putenv_s("LOOK_BIND", argv[++i]);
#else
            setenv("LOOK_BIND", argv[++i], 1);
#endif
        }
        else if (argv[i][0] != '-')
            script = argv[i];  // positional arg: script path
    }

    if (workers <= 0) {
        const char* env = std::getenv("LOOK_WORKERS");
        if (env) workers = std::max(1, std::atoi(env));
        else if (mode == "http" && look::http_fiber_dispatch_enabled()) {
            // Fiber dispatch: while a worker waits on the database / network it runs another request — the worker
            // count equals the available CPUs (like Go's GOMAXPROCS). Measurement: 8 and 32 workers gave the same
            // throughput, p99 16 ms vs 34 ms. It used to be CPU × 4 here too.
            workers = look::available_cpus();
        } else {
            // One thread per request (FastCGI, LOOK_FIBER_DISPATCH=0, Windows): the thread really
            // blocks while waiting → CPU × 4, at most 64.
            workers = look::available_cpus() * 4;
            if (workers > 64) workers = 64;
        }
    }
    if (workers < 2) workers = 2;

    // Fail-loud: under a hard CPU quota, more worker threads than the quota burst
    // past the CFS bandwidth limit and get throttled — the kernel deschedules ALL
    // the container's threads until the next ~100ms period, spiking tail latency
    // (p99 seen jumping to ~40-60ms while p50 stays sub-ms). The default workers =
    // CPUs*4 trips this. The symptom is invisible: p99 high, strace clean, code
    // correct — so warn instead of letting it stay a silent mystery.
    {
        int quota = look::cfs_quota_cpus();
        if (quota > 0 && workers > quota) {
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "workers=" + std::to_string(workers) + " exceeds the CPU quota (" +
                std::to_string(quota) + " core" + (quota == 1 ? "" : "s") + "). Extra worker "
                "threads burst past the CFS quota and get throttled, spiking tail latency (p99). "
                "Set LOOK_WORKERS=" + std::to_string(quota) + " to match the quota.");
        }
    }

    // If an explicitly given session directory (LOOK_SESSION_DIR) belongs to someone else or its permissions are loose,
    // do not start: the sessions in that directory could be read/forged by others.
    {
        std::string why = look::session_dir_startup_problem();
        if (!why.empty()) {
            std::cerr << "Error: LOOK could not start: " << why << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "SESSION", "could not start: " + why);
            return 1;
        }
    }

    // Memory budget (look/mem_budget.h): the process limit is LOOK_MEMORY_LIMIT or automatic (80% of the container,
    // otherwise 1 GiB), the request budget is automatic. It does not start with an invalid value (Go: GOMEMLIMIT error).
    {
        std::string why, describe;
        if (!look::mem_budget_configure(why, describe)) {
            std::cerr << "Error: LOOK could not start: " << why << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", "could not start: " + why);
            return 1;
        }
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", describe);
    }

    // ── --mode http — event loop, Apache bypass ───────────────────────────────
    if (mode == "http") {
        if (workers <= 0) {
            const char* env = std::getenv("LOOK_WORKERS");
            if (env) workers = std::max(1, std::atoi(env));
            else {
                workers = look::available_cpus() * 4;
                if (workers > 64) workers = 64;
            }
        }
        if (workers < 2)  workers = 2;
        if (tcp_port <= 0) tcp_port = 9000;
        try {
            run_http_mode(tcp_port, workers, script);
        } catch (const std::exception& e) {
            // A resource that could not be started during setup (e.g. the database thread pool): write the reason, exit with 1
            // (it used to be an uncaught exception → std::terminate).
            std::cerr << "Error: LOOK could not start: " << e.what() << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", std::string("could not start: ") + e.what());
            return 1;
        }
        return 0;
    }

    {
        // The pool's upper limit is independent of the worker count: if LOOK_DB_POOL_SIZE is given, that; otherwise
        // the default (db::connect: 4 × CPU, 8..64). Connections are opened as needed.
        const char* env = std::getenv("LOOK_DB_POOL_SIZE");
        int pool_sz = (env && env[0]) ? std::max(1, std::min(std::atoi(env), 256)) : 0;
        look::set_db_pool_size(pool_sz);
    }

    if (tcp_port > 0) {
        // ── TCP mode — concurrent dispatch ───────────────────────────────────
        { const char* be = std::getenv("LOOK_FCGI_BIND");
          std::string bindaddr = (be && *be) ? std::string(be) : "127.0.0.1";
          look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI",
            "look-fcgi TCP mode — " + bindaddr + ":" + std::to_string(tcp_port) +
            " | workers=" + std::to_string(workers));
          for (const auto& w : look::weak_setting_warnings(std::getenv, true))   // look/weak_settings.h
              look::Logger::instance().log(look::LogLevel::LOG_WARN, "SECURITY", w); }

        try {
            // Workers FIRST, THEN the port: the constructor starts listening. The port used to be opened first; when the pool
            // could not be set up (at the memory limit) and the process wrote its reason and exited, a client that connected
            // was accepted and left without a response (the counterpart of the startup gate in HTTP mode).
            ThreadPool pool(workers);
            look::FcgiServer server(tcp_port);
            // Graceful shutdown: accepting stops, run() returns; at the end of the scope the pool finishes the queued and in-progress
            // requests and joins (ThreadPool destructor), then finish(0) below.
            look::shutdown::on_begin([&server] { server.stop(); });

            server.run([&pool](look::FcgiConn conn_val) {
                // move conn into heap so the lambda owns it across thread boundary
                auto conn_ptr = std::make_shared<look::FcgiConn>(std::move(conn_val));
                pool.submit([conn_ptr]() mutable {
                    look::FcgiRequest req;
                    // The read timeout for the first request too = the idle time of HTTP mode (LOOK_HTTP_IDLE_MS,
                    // default 30 s; per recv, not total). It did not exist before: every connection that connected and sent
                    // nothing held a worker thread indefinitely.
                    static const int idle_s = [] {
                        const char* e = std::getenv("LOOK_HTTP_IDLE_MS");
                        long ms = (e && *e) ? std::atol(e) : 30000L;
                        return (int)std::max(1L, ms / 1000);
                    }();
                    conn_ptr->set_read_timeout(idle_s);
                    while (conn_ptr->accept(req)) {
                        handle_request(req, *conn_ptr);
                        if (!req.keep_conn) break;
                        if (look::shutdown::requested()) break;   // shutdown: do not wait for the next request
                        // KEEP_CONN: idle timeout while waiting for the next request —
                        // so that idle connections do not pin worker threads indefinitely
                        conn_ptr->set_read_timeout(5);
                    }
                });
            });
            look::shutdown::on_begin(nullptr);   // the server is leaving scope
        } catch (const std::exception& e) {
            look::shutdown::on_begin(nullptr);
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI", e.what());
            return 1;
        }
        if (look::shutdown::requested()) look::shutdown::finish(0);
    } else {
        // ── stdin/stdout mode — mod_fcgid, a single thread is enough ────────────────
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI",
            "look-fcgi pipe mode — waiting for requests via stdin/stdout");

        look::FcgiConn conn;
        look::FcgiRequest req;
        // Graceful shutdown: a signal arriving while waiting for a request exits immediately; a signal arriving while a request is being processed
        // lets the request finish, then exits (lock: there is no race between the two).
        static std::mutex pipe_mtx;
        static bool pipe_busy = false;
        look::shutdown::on_begin([] {
            std::lock_guard<std::mutex> lk(pipe_mtx);
            if (!pipe_busy) look::shutdown::finish(0);
        });
        while (conn.accept(req)) {
            {
                std::lock_guard<std::mutex> lk(pipe_mtx);
                if (look::shutdown::requested()) look::shutdown::finish(0);
                pipe_busy = true;
            }
            handle_request(req, conn);
            {
                std::lock_guard<std::mutex> lk(pipe_mtx);
                pipe_busy = false;
                if (look::shutdown::requested()) look::shutdown::finish(0);
            }
        }
    }

    look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI", "look-fcgi exiting");
    return 0;
}

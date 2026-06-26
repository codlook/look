// look-fcgi.exe — LOOK FastCGI runtime
//
// Iki mod:
//
//   1. stdin/stdout (mod_fcgid — Windows/XAMPP):
//      look-fcgi.exe
//      Apache httpd.conf:
//        FcgidWrapper "C:/xampp/cgi-bin/look-fcgi.exe" .lk
//        AddHandler fcgid-script .lk
//
//   2. TCP (mod_proxy_fcgi — PHP-FPM modeli, her yerde calısır):
//      look-fcgi.exe --port 9000
//      Apache httpd.conf:
//        ProxyPassMatch "^/(.*\.lk.*)$" "fcgi://127.0.0.1:9000/$1"
//      Nginx:
//        fastcgi_pass 127.0.0.1:9000;
//
// Gelistirici kodu hic degismez — sadece deployment modeli degisir.

#include "look/fcgi.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/logger.h"
#include "look/web.h"

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

#ifdef _WIN32
  #include <direct.h>
#else
  #include <unistd.h>
#endif

// ── Thread pool ───────────────────────────────────────────────────────────────

class ThreadPool {
public:
    explicit ThreadPool(int n) : stop_(false) {
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this] {
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
            });
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

// ── FcgiConn implementasyonu ──────────────────────────────────────────────────

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
    type   = hdr[1];
    req_id = (uint16_t)((hdr[2] << 8) | hdr[3]);
    uint16_t content_len = (uint16_t)((hdr[4] << 8) | hdr[5]);
    uint8_t  padding_len = hdr[6];

    content.resize(content_len);
    if (content_len > 0 && !raw_read(content.data(), content_len)) return false;
    if (padding_len > 0) {
        uint8_t pad[255];
        if (!raw_read(pad, padding_len)) return false;
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
    if (p >= end) return 0;
    if (*p >> 7 == 0) return *p++;
    if (p + 4 > end) return 0;
    uint32_t v = ((uint32_t)(p[0] & 0x7F) << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    p += 4;
    return v;
}

void FcgiConn::parse_params(const uint8_t* p, size_t len,
                              std::map<std::string, std::string>& out) {
    const uint8_t* end = p + len;
    while (p < end) {
        uint32_t nlen = read_len(p, end);
        uint32_t vlen = read_len(p, end);
        if (p + nlen + vlen > end) break;
        std::string name (p, p + nlen); p += nlen;
        std::string value(p, p + vlen); p += vlen;
        out[name] = value;
    }
}

bool FcgiConn::accept(FcgiRequest& req) {
    req = FcgiRequest{};
    bool in_request = false;
    while (true) {
        uint8_t type; uint16_t rid;
        std::vector<uint8_t> content;
        if (!read_record(type, rid, content)) return false;

        switch (type) {
            case FCGI_BEGIN_REQUEST:
                if (content.size() >= 8) {
                    req.id        = rid;
                    req.keep_conn = (content[2] & 0x01) != 0;
                    in_request    = true;
                }
                break;
            case FCGI_PARAMS:
                if (in_request && rid == req.id) {
                    if (!content.empty())
                        parse_params(content.data(), content.size(), req.params);
                }
                break;
            case FCGI_STDIN:
                if (in_request && rid == req.id) {
                    if (content.empty()) return true; // request hazir
                    req.body.append(content.begin(), content.end());
                }
                break;
            case FCGI_ABORT_REQUEST:
                end_request(rid, 1);
                req = FcgiRequest{}; in_request = false;
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
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
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

// ── Yardimci fonksiyonlar ─────────────────────────────────────────────────────

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
        ? "{\"ok\":false,\"hata\":\"Sunucu hatasi.\",\"kod\":" + std::to_string(status) + "}\n"
        : "{\"ok\":false,\"hata\":\"" + json_escape_str(msg) + "\",\"kod\":" + std::to_string(status) + "}\n";
    return "Status: " + std::to_string(status) + " " + title + "\r\n"
           "Content-Type: application/json; charset=utf-8\r\n"
           "Access-Control-Allow-Origin: *\r\n\r\n" + body;
}

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open: " + path.string());
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static void set_env_from_params(const std::map<std::string, std::string>& params) {
    for (auto& [k, v] : params) {
#ifdef _WIN32
        SetEnvironmentVariableA(k.c_str(), v.c_str());
#else
        setenv(k.c_str(), v.c_str(), 1);
#endif
    }
}

static std::string resolve_script(const std::map<std::string, std::string>& params) {
    auto get = [&](const std::string& k) {
        auto it = params.find(k); return it != params.end() ? it->second : "";
    };
    auto is_real_lk = [](const std::string& s) {
        // proxy:fcgi://... gibi sahte yollar disla
        if (s.empty()) return false;
        if (s.rfind("proxy:", 0) == 0) return false;
        if (s.rfind("fcgi:",  0) == 0) return false;
        return s.size() > 3 && s.substr(s.size() - 3) == ".lk";
    };

    // 1. PATH_TRANSLATED — CGI Action directive modunda gercek yol gelir
    std::string pt = get("PATH_TRANSLATED");
    if (is_real_lk(pt)) return pt;

    // 2. SCRIPT_FILENAME — gercek Windows yolu ise kullan
    std::string sf = get("SCRIPT_FILENAME");
    if (is_real_lk(sf)) return sf;

    // 3. DOCUMENT_ROOT + SCRIPT_NAME — mod_proxy_fcgi RewriteRule [P] modunda
    //    ornek: SCRIPT_NAME=/index.lk, DOCUMENT_ROOT=C:/xampp/htdocs
    std::string doc = get("DOCUMENT_ROOT");
    std::string sn  = get("SCRIPT_NAME");
    if (!doc.empty() && !sn.empty() && sn.size() > 3 && sn.substr(sn.size()-3) == ".lk") {
        return doc + sn;
    }

    // 4. PATH_INFO'dan .lk coz (Apache Action + PATH_INFO modeli)
    std::string pi = get("PATH_INFO");
    if (!doc.empty() && !pi.empty()) {
        size_t pos = pi.find(".lk");
        if (pos != std::string::npos) return doc + pi.substr(0, pos + 3);
    }

    // 5. Fallback: DOCUMENT_ROOT/index.lk — RewriteRule [P] ile gelen tum istekler
    //    REQUEST_URI routing icin WebContext'e gecer, script hep index.lk
    if (!doc.empty()) {
        std::string idx = doc;
        // Windows: ters slash normalize et
        for (char& c : idx) if (c == '\\') c = '/';
        if (idx.back() != '/') idx += '/';
        idx += "index.lk";
        return idx;
    }

    return "";
}

// ── Warm start state — process boyunca yasayan interpreter ───────────────────

struct WarmApp {
    std::string                        script_path;
    std::unique_ptr<look::Interpreter> interp;
    std::unique_ptr<look::Program>     program;    // AST — closures buna pointer tutar
    std::ostringstream                 setup_out;  // interpreter omrunde yasayan stream
    fs::file_time_type                 mtime;
    bool                               ready = false;
};

static WarmApp          g_app;
static std::shared_mutex g_app_mutex;  // shared = concurrent dispatch, exclusive = setup/reload

// FCGI params'tan WebContext doldur (env var bagimsiz)
static look::WebContext make_web_context(const look::FcgiRequest& req) {
    auto param = [&](const std::string& k) -> std::string {
        auto it = req.params.find(k);
        return it != req.params.end() ? it->second : "";
    };

    look::WebContext web;

    web.method = param("REQUEST_METHOD");
    if (web.method.empty()) web.method = "GET";

    // Path: REQUEST_URI'dan al
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
    if (web.content_type.find("application/x-www-form-urlencoded") != std::string::npos) {
        web.post_params = look::WebContext::parse_query(req.body);
    } else if (web.content_type.find("multipart/form-data") != std::string::npos) {
        size_t bpos = web.content_type.find("boundary=");
        if (bpos != std::string::npos)
            web.parse_multipart(web.content_type.substr(bpos + 9));
    }

    web.remote_addr = param("REMOTE_ADDR");
    std::string ck  = param("HTTP_COOKIE");
    if (!ck.empty()) web.cookies_in = look::WebContext::parse_cookies(ck);

    return web;
}

// ── Setup fazı — exclusive lock altında çalışır ──────────────────────────────

static void run_setup(const std::string& script_path, fs::file_time_type mtime,
                      look::FcgiRequest& req, look::FcgiConn& conn) {
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI",
        "Warm start: " + script_path);
    look::clear_db_pools(); // eski bağlantıları kapat — yeni setup kendi pool'unu açar

    fs::path script_dir = fs::path(script_path).parent_path();
    if (!script_dir.empty()) {
#ifdef _WIN32
        _wchdir(script_dir.wstring().c_str());
#else
        (void)chdir(script_dir.string().c_str());
#endif
    }

    std::string source = read_file(script_path);

    static look::WebContext setup_ctx;
    setup_ctx         = look::WebContext{};
    setup_ctx.method  = "__SETUP__";

    g_app.setup_out.str(""); g_app.setup_out.clear();
    auto new_interp = std::make_unique<look::Interpreter>(g_app.setup_out);
    new_interp->set_setup_mode(true);
    new_interp->set_web_context(&setup_ctx);

    look::Lexer  lexer(source);
    auto         tokens = lexer.scan_tokens();
    look::Parser parser(std::move(tokens));
    std::unique_ptr<look::Program> program;
    try {
        program = parser.parse();
    } catch (const look::LookParseError& e) {
        auto err = e;
        if (err.file.empty()) err.file = script_path;
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI", err.format());
        conn.write_stdout(req.id, build_error_response(500, "Parse Error", err.format()));
        conn.end_request(req.id, 0);
        return;
    }

    new_interp->set_file(script_path);

    try {
        new_interp->interpret(*program);
    } catch (const look::ExitException&) {
    } catch (const look::RouteMatchedException&) {
    } catch (const look::LookRuntimeError& e) {
        auto err = e;
        if (err.location.file.empty()) err.location.file = script_path;
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "FCGI",
            std::string("Setup warning: ") + err.format());
    } catch (const std::exception& e) {
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "FCGI",
            std::string("Setup warning (non-fatal): ") + e.what());
    }

    new_interp->set_setup_mode(false);

    g_app.interp      = std::move(new_interp);
    g_app.program     = std::move(program);
    g_app.script_path = script_path;
    g_app.mtime       = mtime;
    g_app.ready       = true;
}

// ── Request isleme — concurrent dispatch modeli ───────────────────────────────

static void handle_request(look::FcgiRequest& req, look::FcgiConn& conn) {
    set_env_from_params(req.params);

    std::string script_path = resolve_script(req.params);
    if (script_path.empty()) {
        conn.write_stdout(req.id, build_error_response(400, "Bad Request", "No script."));
        conn.end_request(req.id, 1);
        return;
    }

    try {
        // ── Hot reload kontrolü ──────────────────────────────────────────────
        fs::file_time_type mtime{};
        try { mtime = fs::last_write_time(script_path); } catch (...) {}

        {
            // Hızlı shared okuma — setup gerekli mi?
            std::shared_lock<std::shared_mutex> rlock(g_app_mutex);
            bool needs_setup = !g_app.ready
                            || g_app.script_path != script_path
                            || mtime != g_app.mtime;

            if (needs_setup) {
                // Exclusive lock al — diğer thread'ler bu noktada bekler
                rlock.unlock();
                std::unique_lock<std::shared_mutex> wlock(g_app_mutex);
                // Double-check: başka thread setup yaptı mı?
                needs_setup = !g_app.ready
                           || g_app.script_path != script_path
                           || mtime != g_app.mtime;
                if (needs_setup) {
                    run_setup(script_path, mtime, req, conn);
                    if (!g_app.ready) return; // parse error — response gönderildi
                }
            }
        }

        // ── Dispatch fazı: shared lock, concurrent ───────────────────────────
        std::shared_lock<std::shared_mutex> rlock(g_app_mutex);

        // Per-request interpreter copy — her thread kendi state'ini taşır
        auto interp = g_app.interp->make_dispatch_copy();
        bool is_direct = (interp->get_route_count() == 0);

        look::WebContext web;
        std::ostringstream output;

        try {
            web = make_web_context(req);
            interp->set_output(output);
            interp->set_web_context(&web);

            // Connection pool'dan bu request için bağlantı al
            look::acquire_thread_connections();

            if (is_direct) {
                interp->interpret(*g_app.program);
            } else {
                interp->dispatch_routes();
            }
        } catch (const look::RouteMatchedException&) {
        } catch (const look::ExitException&) {
        } catch (const look::LookRuntimeError& e) {
            look::release_thread_connections();
            auto err = e;
            if (err.location.file.empty()) err.location.file = script_path;
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI", err.format());
            if (output.str().empty() && !web.headers_sent) {
                conn.write_stdout(req.id,
                    build_error_response(503, "Service Unavailable", err.format()));
                conn.end_request(req.id, 0);
                return;
            }
        } catch (const std::exception& e) {
            look::release_thread_connections();
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI",
                std::string("Dispatch error: ") + e.what());
            if (output.str().empty() && !web.headers_sent) {
                conn.write_stdout(req.id,
                    build_error_response(503, "Service Unavailable", e.what()));
                conn.end_request(req.id, 0);
                return;
            }
        }

        look::release_thread_connections();
        conn.write_stdout(req.id, web.build_headers() + output.str());

    } catch (const std::exception& e) {
        {
            std::unique_lock<std::shared_mutex> wlock(g_app_mutex);
            g_app.ready = false;
        }
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
    // --port NNNN     → TCP modu (fcgi) veya http modu
    // --workers N     → concurrent dispatch thread sayısı (default: CPU çekirdeği)
    // --mode fcgi     → FastCGI modu (default)
    // --mode http     → HTTP/1.1 modu, Apache bypass, event loop
    // argumansiz      → stdin/stdout modu (mod_fcgid)
    int         tcp_port = 0;
    int         workers  = 0;
    std::string mode     = "fcgi";
    std::string script   = "index.lk";

    // version / --version / -v → versiyon bas ve çık
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
        std::cout << "LOOK 1.0.0 (" << platform << "/" << arch << ")\n";
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--port") == 0 || std::strcmp(argv[i], "-p") == 0)
            && i + 1 < argc)
            tcp_port = std::atoi(argv[++i]);
        else if ((std::strcmp(argv[i], "--workers") == 0 || std::strcmp(argv[i], "-w") == 0)
            && i + 1 < argc)
            workers = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc)
            script = argv[++i];
    }

    if (workers <= 0)
        workers = (int)std::thread::hardware_concurrency();
    if (workers < 2) workers = 2;

    // ── --mode http — event loop, Apache bypass ───────────────────────────────
    if (mode == "http") {
        if (workers <= 0) workers = (int)std::thread::hardware_concurrency();
        if (workers < 2)  workers = 2;
        if (tcp_port <= 0) tcp_port = 9000;
        run_http_mode(tcp_port, workers, script);
        return 0;
    }

    look::set_db_pool_size(workers); // pool size = worker count — 1:1 guaranteed availability

    if (tcp_port > 0) {
        // ── TCP modu — concurrent dispatch ───────────────────────────────────
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI",
            "look-fcgi TCP mode — 127.0.0.1:" + std::to_string(tcp_port) +
            " | workers=" + std::to_string(workers));

        try {
            look::FcgiServer server(tcp_port);
            ThreadPool pool(workers);

            server.run([&pool](look::FcgiConn conn_val) {
                // move conn into heap so the lambda owns it across thread boundary
                auto conn_ptr = std::make_shared<look::FcgiConn>(std::move(conn_val));
                pool.submit([conn_ptr]() mutable {
                    look::FcgiRequest req;
                    while (conn_ptr->accept(req)) {
                        handle_request(req, *conn_ptr);
                        if (!req.keep_conn) break;
                    }
                });
            });
        } catch (const std::exception& e) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "FCGI", e.what());
            return 1;
        }
    } else {
        // ── stdin/stdout modu — mod_fcgid, tek thread yeterli ────────────────
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI",
            "look-fcgi pipe mode — waiting for requests via stdin/stdout");

        look::FcgiConn conn;
        look::FcgiRequest req;
        while (conn.accept(req)) {
            handle_request(req, conn);
        }
    }

    look::Logger::instance().log(look::LogLevel::LOG_INFO, "FCGI", "look-fcgi exiting");
    return 0;
}

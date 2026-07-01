// look-fcgi --mode http
//
// HTTP/1.1 modunda doğrudan TCP dinler — Apache/FastCGI bypass.
// Nginx opsiyonel reverse proxy olarak önünde durabilir.
//
// Mevcut --mode fcgi (default) hiç değişmez.
// Aynı .lk kodu her iki modda çalışır.

#include "look/http_server.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/sse.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <atomic>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <deque>
#include <unordered_map>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <unistd.h>
#  include <signal.h>
#endif

namespace fs = std::filesystem;

// ── HTTP rate limiter (sliding window, per-IP) ────────────────────────────────
// LOOK_RATE_LIMIT_RPM: max requests per minute per IP (default 0 = disabled)
// LOOK_RATE_LIMIT_BURST: max concurrent burst within window (default = RPM)
//
// Uses a deque of timestamps per IP (sliding window).
// On limit: returns true (caller should send 429).

static int http_rate_limit_rpm() {
    static int v = []() {
        const char* e = std::getenv("LOOK_RATE_LIMIT_RPM");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 0; // 0 = disabled
    }();
    return v;
}

struct RateLimiter {
    struct IpState {
        std::deque<std::chrono::steady_clock::time_point> hits;
    };
    std::unordered_map<std::string, IpState> states;
    std::mutex mtx;

    // Returns true if the IP is rate-limited (should be rejected).
    bool check(const std::string& ip, int rpm) {
        if (rpm <= 0) return false;
        auto now = std::chrono::steady_clock::now();
        auto window = std::chrono::seconds(60);
        std::lock_guard<std::mutex> lk(mtx);
        auto& s = states[ip];
        // Evict entries outside the window
        while (!s.hits.empty() && now - s.hits.front() > window)
            s.hits.pop_front();
        if ((int)s.hits.size() >= rpm) return true;
        s.hits.push_back(now);
        return false;
    }
};

static RateLimiter g_rate_limiter;

// ── Shared hot-reload state (mirrors WarmApp in fcgi_main) ───────────────────

struct HttpApp {
    // ── Tree-walk interpreter (her zaman mevcut — fallback) ───────────────────
    std::unique_ptr<look::Interpreter>       interp;
    std::unique_ptr<look::Program>           program;
    std::ostringstream                       setup_out;

    // ── Bytecode VM (opsiyonel — derleme başarılıysa kullanılır) ──────────────
    bool                                     use_bytecode = false;
    std::unique_ptr<look::CompiledProgram>   compiled;

    // VM route registry: setup fazında VM çalıştırılarak doldurulur.
    // pattern = "GET:/path/{id}", closure = compiled closure ptr (BYTECODE_FN value)
    std::vector<std::pair<std::string, look::Value>> vm_routes;
    // VM'nin dispatch için hazır olduğunu gösterir.
    bool                                     vm_routes_ready = false;
    // Setup VM globals: db_check/json_ok/json_hata/admin_kontrol closure'ları + $conn pool key
    std::unordered_map<std::string, look::Value> vm_setup_globals;

    // Interpreter'ın shared globals/struct_defs — VM SharedState için read-only ptr
    // (make_dispatch_copy ile ayrı interpreter kopyasına da erişilebilir)

    fs::path                                 script_path;
    fs::file_time_type                       mtime;
};

static HttpApp                  g_http_app;
static std::shared_mutex        g_http_mutex;
static std::atomic<bool>        g_http_ready{false};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("Dosya açılamadı: " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

static void run_setup_http(const fs::path& script) {
    look::clear_db_pools();

    auto src = read_file(script);

    look::Lexer  lexer(src);
    auto tokens = lexer.scan_tokens();

    look::Parser parser(std::move(tokens));
    auto program = parser.parse();

    g_http_app.setup_out.str("");
    g_http_app.setup_out.clear();

    static look::WebContext setup_ctx;
    setup_ctx        = look::WebContext{};
    setup_ctx.method = "__SETUP__";

    auto interp = std::make_unique<look::Interpreter>(g_http_app.setup_out);
    interp->set_file(script.string());
    interp->set_web_context(&setup_ctx);
    interp->set_setup_mode(true);
    interp->interpret(*program);
    interp->set_setup_mode(false);

    g_http_app.program     = std::move(program);
    g_http_app.interp      = std::move(interp);
    g_http_app.script_path = script;

    // ── Bytecode compile — fallback'li ────────────────────────────────────────
    // LOOK_BYTECODE=0 env ile devre dışı bırakılabilir (debug)
    static const bool bytecode_enabled = ([](){
        const char* e = std::getenv("LOOK_BYTECODE");
        return e == nullptr || std::string(e) != "0";
    })();

    g_http_app.use_bytecode = false;
    std::cerr << "[BYTECODE] enabled=" << bytecode_enabled << "\n";
    if (bytecode_enabled) {
        try {
            std::cerr << "[BYTECODE] Compiling...\n";
            auto compiled = std::make_unique<look::CompiledProgram>(
                look::Compiler::compile(*g_http_app.program));
            g_http_app.compiled     = std::move(compiled);
            g_http_app.use_bytecode = true;
            std::cerr << "[BYTECODE] OK — VM modu aktif\n";
            look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
                "Bytecode compile OK — VM modu aktif");
        } catch (const look::LookCompileError& e) {
            std::cerr << "[BYTECODE] LookCompileError: " << e.what() << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                std::string("Bytecode compile başarısız, interpreter fallback: ") + e.what());
        } catch (const std::exception& e) {
            std::cerr << "[BYTECODE] exception: " << e.what() << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                std::string("Bytecode compile hata, interpreter fallback: ") + e.what());
        } catch (...) {
            std::cerr << "[BYTECODE] unknown exception\n";
        }

    // ── VM Setup Fazı: bytecode'u çalıştır, route'ları kaydet ────────────────
    g_http_app.vm_routes.clear();
    g_http_app.vm_routes_ready = false;
    if (g_http_app.use_bytecode && g_http_app.compiled) {
        try {
            // Setup builtins: route() (index 22) ve temel veri fonksiyonları
            // Diğer builtin'ler setup'ta çağrılmaz (db::query vb. HTTP context ister)
            auto vm_routes_ptr = &g_http_app.vm_routes;
            std::vector<look::BuiltinFn> setup_builtins(148);

            // print/write — setup çıktısı için (0, 1)
            auto* setup_out_ptr = &g_http_app.setup_out;
            setup_builtins[0] = [setup_out_ptr](std::vector<look::Value>& args) -> look::Value {
                for (auto& a : args) *setup_out_ptr << a.to_string();
                return look::Value();
            };
            setup_builtins[1] = setup_builtins[0];

            // count (2)
            setup_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value(0);
                auto& v = args[0];
                if (v.type() == look::Value::ARRAY)
                    return look::Value((int)v.as_array()->size());
                if (v.type() == look::Value::STRING)
                    return look::Value((int)v.as_string().size());
                return look::Value(0);
            };

            // push (3) — setup'ta $routes_list push'ları için
            setup_builtins[3] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                args[0].as_array()->push_back(args[1]);
                return look::Value();
            };
            // pop (4)
            setup_builtins[4] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty() || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                auto& v = *args[0].as_array();
                if (v.empty()) return look::Value();
                look::Value r = v.back(); v.pop_back(); return r;
            };

            // str (5)
            setup_builtins[5] = [](std::vector<look::Value>& args) -> look::Value {
                return look::Value(args.empty() ? "" : args[0].to_string());
            };
            // int (6)
            setup_builtins[6] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value(0);
                try { return look::Value((int)std::stoll(args[0].to_string())); }
                catch (...) { return look::Value(0); }
            };
            // float (7)
            setup_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value(0.0);
                try { return look::Value(std::stod(args[0].to_string())); }
                catch (...) { return look::Value(0.0); }
            };

            // log::info/warn/error/debug (43-46) — setup logları
            for (int i = 43; i <= 46; ++i) {
                std::string level_str = (i==43?"INFO":i==44?"WARN":i==45?"ERROR":"DEBUG");
                setup_builtins[i] = [level_str](std::vector<look::Value>& args) -> look::Value {
                    auto lvl = (level_str=="INFO") ? look::LogLevel::LOG_INFO
                             : (level_str=="WARN") ? look::LogLevel::LOG_WARN
                             : (level_str=="ERROR")? look::LogLevel::LOG_ERROR
                             :                       look::LogLevel::LOG_DEBUG;
                    look::Logger::instance().log(lvl, "VM-setup",
                        args.empty() ? "" : args[0].to_string());
                    return look::Value();
                };
            }

            // db::connect (setup'ta çağrılır) — interpreter fallback ile
            // NOT: db:: fonksiyonları setup'ta interpreter tarafından çalıştırıldı.
            // VM setup fazında db:: yok — sadece route() kayıt önemli.

            // route() — index 22: pattern + closure kaydet
            setup_builtins[22] = [vm_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                // route("GET", "/path", closure) OR route("404", closure)
                if (args.size() >= 3) {
                    // 3-arg form: method, pattern, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = args[1].to_string();
                    if (args[2].type() == look::Value::BYTECODE_FN) {
                        vm_routes_ptr->push_back({method + ":" + pattern, args[2]});
                    }
                } else if (args.size() >= 2) {
                    // 2-arg form: "404" + fn
                    std::string method  = args[0].to_string();
                    if (args[1].type() == look::Value::BYTECODE_FN) {
                        vm_routes_ptr->push_back({method + ":/", args[1]});
                    }
                }
                return look::Value();
            };

            // db::connect (56) — interpreter'ın mevcut pool key'ini döndür (aynı DSN için yeni
            // pool açmaz). VM route closures bu key'i capture eder; acquire_thread_connections()
            // bu pool'dan bağlantı alır. Farklı DSN için yeni pool açmak gerekmez (index.lk tek DSN).
            setup_builtins[56] = [](std::vector<look::Value>& args) -> look::Value {
                // Interpreter'ın $conn global'ını al — setup'ta oluşturulmuş pool key
                look::Value existing = g_http_app.interp->get_global("conn");
                if (existing.type() == look::Value::STRING) return existing;
                // Fallback: interpreter'ın db::connect'ini çağır (interpreter pool'una ekler)
                auto f = g_http_app.interp->get_module_fn("db", "connect");
                if (!f) return look::Value();
                std::vector<look::Value> a = args;
                return f(a);
            };
            // channel (57) — LookChannel oluşturur (setup'ta da kullanılabilir: $hub = channel())
            setup_builtins[57] = [](std::vector<look::Value>& args) -> look::Value {
                int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
                if (buf <= 0) buf = (1 << 20); // 0 = sınırsız ~ 1M
                return look::Value(std::make_shared<look::LookChannel>(buf));
            };
            // env (58) — setup'ta DB DSN oluşturmak için çağrılır (.env-aware)
            setup_builtins[58] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value();
                std::string key = args[0].to_string();
                std::string def = (args.size() >= 2) ? args[1].to_string() : "";
                return look::Value(look::look_get_env(key, def));
            };
            // config (59) — setup'ta kullanılabilir
            {
                auto f = g_http_app.interp->get_module_fn("", "config");
                if (f) setup_builtins[59] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args;
                    return f(a);
                };
            }
            // date::timestamp (105) + file:: (106-112) — setup'ta çağrılabilir
            {
                auto f = g_http_app.interp->get_module_fn("date", "timestamp");
                if (f) setup_builtins[105] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args; return f(a);
                };
            }
            for (auto& [idx, mod, fn] : std::vector<std::tuple<int,std::string,std::string>>{
                {106,"file","read"},{107,"file","put"},{108,"file","append"},
                {109,"file","exists"},{110,"file","remove"},{111,"file","size"},{112,"file","store"}
            }) {
                auto f = g_http_app.interp->get_module_fn(mod, fn);
                if (f) setup_builtins[idx] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args; return f(a);
                };
            }

            look::VM::SharedState setup_sh;
            setup_sh.builtins = &setup_builtins;
            std::ostringstream vm_setup_out;
            look::VM setup_vm(setup_sh, vm_setup_out);
            setup_vm.execute(*g_http_app.compiled);

            // Setup globals'ı sakla: db_check/json_ok/json_hata gibi named fn closure'lar +
            // $conn pool key — dispatch VM'e set_globals() ile verilecek
            g_http_app.vm_setup_globals = setup_vm.get_globals();

            if (!g_http_app.vm_routes.empty()) {
                g_http_app.vm_routes_ready = true;
                std::cerr << "[BYTECODE] VM routes: " << g_http_app.vm_routes.size() << " kayıtlandı\n";
                look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
                    "VM dispatch hazır — " + std::to_string(g_http_app.vm_routes.size()) + " route");
            } else {
                std::cerr << "[BYTECODE] VM route yok — interpreter dispatch kullanılacak\n";
            }
        } catch (const look::LookVmError& e) {
            std::cerr << "[BYTECODE] VM setup LookVmError: " << e.what() << "\n";
            g_http_app.vm_routes_ready = false;
        } catch (const std::bad_function_call& e) {
            std::cerr << "[BYTECODE] VM setup bad_function_call: " << e.what() << "\n";
            g_http_app.vm_routes_ready = false;
        } catch (const std::exception& e) {
            std::cerr << "[BYTECODE] VM setup exception (" << typeid(e).name() << "): " << e.what() << "\n";
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                std::string("VM setup hatası, interpreter fallback: ") + e.what());
            g_http_app.vm_routes_ready = false;
        }
    }
    }
    g_http_app.mtime       = fs::last_write_time(script);

    g_http_ready = true;
}

// ── WebContext from HttpRequest ───────────────────────────────────────────────

static look::WebContext make_web_ctx(const look::HttpRequest& req) {
    look::WebContext ctx;
    ctx.method       = req.method;
    ctx.path         = req.path;
    ctx.query_string = req.query_string;
    ctx.body         = req.body;

    // Parse query string
    ctx.get_params = look::WebContext::parse_query(req.query_string);

    // Parse cookies
    auto it = req.headers.find("cookie");
    if (it != req.headers.end())
        ctx.cookies_in = look::WebContext::parse_cookies(it->second);

    // Content-type
    auto ict = req.headers.find("content-type");
    if (ict != req.headers.end()) ctx.content_type = ict->second;

    // Remote addr
    auto iip = req.headers.find("x-forwarded-for");
    if (iip != req.headers.end()) ctx.remote_addr = iip->second;

    // POST body parse
    if (req.method == "POST" && !req.body.empty()) {
        if (ctx.content_type.find("application/x-www-form-urlencoded") != std::string::npos)
            ctx.post_params = look::WebContext::parse_query(req.body);
    }

    return ctx;
}

// ── Request handler called by HttpServer worker threads ───────────────────────

static void http_handler(const look::HttpRequest& req, look::HttpResponse& resp) {
    // Hot reload check
    {
        std::shared_lock<std::shared_mutex> sl(g_http_mutex);
        if (!g_http_ready) {
            resp.status_code = 503;
            resp.status_text = "Service Unavailable";
            resp.body = "{\"ok\":false,\"hata\":\"Başlatılıyor\"}";
            return;
        }

        auto cur_mtime = fs::last_write_time(g_http_app.script_path);
        bool need_reload = (cur_mtime != g_http_app.mtime);
        sl.unlock();

        if (need_reload) {
            std::unique_lock<std::shared_mutex> ul(g_http_mutex);
            // Double-check under exclusive lock
            auto mtime2 = fs::last_write_time(g_http_app.script_path);
            if (mtime2 != g_http_app.mtime) {
                try {
                    run_setup_http(g_http_app.script_path);
                    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Hot reload: " + g_http_app.script_path.string());
                } catch (const std::exception& e) {
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", std::string("Hot reload hatası: ") + e.what());
                }
            }
        }
    }

    // ── Rate limit check ─────────────────────────────────────────────────────
    {
        // Prefer X-Real-IP (set by nginx), then X-Forwarded-For, then fallback
        std::string client_ip;
        auto ireal = req.headers.find("x-real-ip");
        if (ireal != req.headers.end()) client_ip = ireal->second;
        else {
            auto ixff = req.headers.find("x-forwarded-for");
            if (ixff != req.headers.end()) {
                client_ip = ixff->second;
                auto comma = client_ip.find(',');
                if (comma != std::string::npos) client_ip = client_ip.substr(0, comma);
            }
        }
        if (!client_ip.empty() &&
            g_rate_limiter.check(client_ip, http_rate_limit_rpm())) {
            res.status_code = 429;
            res.status_text = "Too Many Requests";
            res.headers["Content-Type"] = "text/plain";
            res.headers["Retry-After"]  = "60";
            res.body = "429 Too Many Requests";
            return;
        }
    }

    // Dispatch
    std::shared_lock<std::shared_mutex> sl(g_http_mutex);
    // vm_routes_ready: VM'nin dispatch_routes() kullanabilmesi için
    // native function wiring tamamlanmış olmalı (Phase 16.5 Adım 5.5)
    bool use_bc = g_http_app.use_bytecode && g_http_app.compiled && g_http_app.vm_routes_ready;

    look::WebContext web = make_web_ctx(req);
    std::ostringstream output;

    auto t_copy_start    = std::chrono::steady_clock::now();
    auto t_copy_end      = t_copy_start;
    auto t_dispatch_start = t_copy_start;

    if (use_bc) {
        // ── VM path ───────────────────────────────────────────────────────────
        // Per-request: interpreter copy → set_web_context → extract module fns as builtins
        t_copy_start = std::chrono::steady_clock::now();
        auto copy = g_http_app.interp->make_dispatch_copy();
        t_copy_end = std::chrono::steady_clock::now();
        copy->set_output(output);
        copy->set_web_context(&web);

        // VM routes'u shared_lock altında alıyoruz (g_http_app read-only erişim)
        std::vector<std::pair<std::string, look::Closure*>> route_closures;
        route_closures.reserve(g_http_app.vm_routes.size());
        for (auto& [pat, val] : g_http_app.vm_routes)
            if (val.type() == look::Value::BYTECODE_FN)
                route_closures.push_back({pat, val.as_bytecode_fn().get()});
        sl.unlock();

        // Per-request builtins: module fonksiyonları interpreter copy'den alınır
        // (copy->set_web_context(&web) çağrıldığında modules_ yeni web'e bind edildi)
        std::vector<look::BuiltinFn> req_builtins(148);

        // print/write (0, 1)
        req_builtins[0] = [&output](std::vector<look::Value>& args) -> look::Value {
            for (auto& a : args) output << a.to_string();
            return look::Value();
        };
        req_builtins[1] = req_builtins[0];

        // count (2)
        req_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0);
            auto& v = args[0];
            if (v.type() == look::Value::ARRAY)
                return look::Value((int)v.as_array()->size());
            if (v.type() == look::Value::STRING)
                return look::Value((int)v.as_string().size());
            return look::Value(0);
        };

        // str/int/float/bool (5-8)
        req_builtins[5] = [](std::vector<look::Value>& args) -> look::Value {
            return look::Value(args.empty() ? "" : args[0].to_string());
        };
        req_builtins[6] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0);
            try { return look::Value((int)std::stoll(args[0].to_string())); }
            catch (...) { return look::Value(0); }
        };
        req_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0.0);
            try { return look::Value(std::stod(args[0].to_string())); }
            catch (...) { return look::Value(0.0); }
        };
        req_builtins[8] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(false);
            auto& v = args[0];
            if (v.type() == look::Value::BOOL) return v;
            if (v.type() == look::Value::INT) return look::Value(v.as_int() != 0);
            if (v.type() == look::Value::FLOAT) return look::Value(v.as_float() != 0.0);
            if (v.type() == look::Value::STRING) return look::Value(!v.as_string().empty());
            if (v.type() == look::Value::NONE) return look::Value(false);
            return look::Value(true);
        };

        // string (9) — alias for str
        req_builtins[9] = req_builtins[5];

        // route() dispatch'te no-op (22)
        req_builtins[22] = [](std::vector<look::Value>&) -> look::Value { return look::Value(); };

        // json::encode / json::decode (23, 24) — interpreter copy'den al
        auto wire_module_fn = [&](int idx, const std::string& mod, const std::string& fn) {
            auto f = copy->get_module_fn(mod, fn);
            if (f) {
                req_builtins[idx] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args;
                    return f(a);
                };
            }
        };
        wire_module_fn(23, "json",     "encode");
        wire_module_fn(24, "json",     "decode");
        wire_module_fn(25, "response", "status");
        wire_module_fn(26, "response", "header");
        wire_module_fn(27, "response", "redirect");
        wire_module_fn(28, "request",  "method");
        wire_module_fn(29, "request",  "get");
        wire_module_fn(30, "request",  "post");
        wire_module_fn(31, "request",  "json");
        wire_module_fn(32, "request",  "body");
        wire_module_fn(33, "request",  "path");
        wire_module_fn(34, "request",  "ip");
        wire_module_fn(35, "request",  "param");
        wire_module_fn(36, "request",  "all");
        wire_module_fn(37, "db",       "query");
        wire_module_fn(38, "db",       "exec");
        wire_module_fn(39, "db",       "last_id");
        wire_module_fn(40, "db",       "affected");
        wire_module_fn(41, "db",       "col");
        wire_module_fn(42, "db",       "close");
        wire_module_fn(43, "log",      "info");
        wire_module_fn(44, "log",      "warn");
        wire_module_fn(45, "log",      "error");
        wire_module_fn(46, "log",      "debug");
        wire_module_fn(47, "log",      "query");
        wire_module_fn(48, "session",  "start");
        wire_module_fn(49, "session",  "get");
        wire_module_fn(50, "session",  "set");
        wire_module_fn(51, "session",  "destroy");
        wire_module_fn(52, "cookie",   "get");
        wire_module_fn(53, "cookie",   "set");
        wire_module_fn(54, "cookie",   "delete");
        wire_module_fn(55, "cookie",   "has");
        wire_module_fn(56, "db",       "connect");
        // date:: (60-69, 104)
        wire_module_fn(60, "date",     "now");
        wire_module_fn(61, "date",     "today");
        wire_module_fn(62, "date",     "format");
        wire_module_fn(63, "date",     "parse");
        wire_module_fn(64, "date",     "add");
        wire_module_fn(65, "date",     "sub");
        wire_module_fn(66, "date",     "diff");
        wire_module_fn(67, "date",     "from_timestamp");
        wire_module_fn(68, "date",     "is_valid");
        wire_module_fn(69, "date",     "weekday");
        wire_module_fn(104,"date",     "week");
        wire_module_fn(105,"date",     "timestamp");
        // file:: (106-112)
        wire_module_fn(106,"file",     "read");
        wire_module_fn(107,"file",     "put");
        wire_module_fn(108,"file",     "append");
        wire_module_fn(109,"file",     "exists");
        wire_module_fn(110,"file",     "remove");
        wire_module_fn(111,"file",     "size");
        wire_module_fn(112,"file",     "store");
        // string:: (70-89)
        wire_module_fn(70, "string",   "upper");
        wire_module_fn(71, "string",   "lower");
        wire_module_fn(72, "string",   "trim");
        wire_module_fn(73, "string",   "replace");
        wire_module_fn(74, "string",   "contains");
        wire_module_fn(75, "string",   "substr");
        wire_module_fn(76, "string",   "split");
        wire_module_fn(77, "string",   "join");
        wire_module_fn(78, "string",   "len");
        wire_module_fn(79, "string",   "starts_with");
        wire_module_fn(80, "string",   "ends_with");
        wire_module_fn(81, "string",   "reverse");
        wire_module_fn(82, "string",   "repeat");
        wire_module_fn(83, "string",   "index_of");
        wire_module_fn(84, "string",   "pad_left");
        wire_module_fn(85, "string",   "pad_right");
        wire_module_fn(86, "string",   "slugify");
        wire_module_fn(87, "string",   "random");
        wire_module_fn(88, "string",   "ltrim");
        wire_module_fn(89, "string",   "rtrim");
        // auth:: (90-91)
        wire_module_fn(90, "auth",     "hash");
        wire_module_fn(91, "auth",     "verify");
        // validator:: (92-97)
        wire_module_fn(92, "validator","required");
        wire_module_fn(93, "validator","email");
        wire_module_fn(94, "validator","integer");
        wire_module_fn(95, "validator","numeric");
        wire_module_fn(96, "validator","min");
        wire_module_fn(97, "validator","max");
        // html:: (98-100)
        wire_module_fn(98, "html",     "escape");
        wire_module_fn(99, "html",     "attr");
        wire_module_fn(100,"html",     "strip");
        // math:: (101-103)
        wire_module_fn(101,"math",     "abs");
        wire_module_fn(102,"math",     "floor");
        wire_module_fn(103,"math",     "ceil");
        // channel() (57) — req_builtins'de doğrudan oluştur
        req_builtins[57] = [](std::vector<look::Value>& args) -> look::Value {
            int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
            if (buf <= 0) buf = (1 << 20);
            return look::Value(std::make_shared<look::LookChannel>(buf));
        };
        // env() (58) ve config() (59)
        req_builtins[58] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value();
            std::string key = args[0].to_string();
            std::string def = (args.size() >= 2) ? args[1].to_string() : "";
            return look::Value(look::look_get_env(key, def));
        };
        {
            auto f_cfg = copy->get_module_fn("", "config");
            if (f_cfg) req_builtins[59] = [f_cfg](std::vector<look::Value>& args) -> look::Value {
                std::vector<look::Value> a = args; return f_cfg(a);
            };
        }

        // ── VM parity: wire indices 113-147 ─────────────────────────────────
        // http:: (113-119)
        wire_module_fn(113,"http",     "get");
        wire_module_fn(114,"http",     "post");
        wire_module_fn(115,"http",     "put");
        wire_module_fn(116,"http",     "delete");
        wire_module_fn(117,"http",     "patch");
        wire_module_fn(118,"http",     "head");
        wire_module_fn(119,"http",     "request");
        // template:: (120-123)
        wire_module_fn(120,"template", "render");
        wire_module_fn(121,"template", "include");
        wire_module_fn(122,"template", "block");
        wire_module_fn(123,"template", "extends");
        // cache:: (124-128)
        wire_module_fn(124,"cache",    "get");
        wire_module_fn(125,"cache",    "set");
        wire_module_fn(126,"cache",    "has");
        wire_module_fn(127,"cache",    "delete");
        wire_module_fn(128,"cache",    "flush");
        // queue:: (129-132)
        wire_module_fn(129,"queue",    "push");
        wire_module_fn(130,"queue",    "pop");
        wire_module_fn(131,"queue",    "size");
        wire_module_fn(132,"queue",    "flush");
        // mail:: (133-134)
        wire_module_fn(133,"mail",     "send");
        wire_module_fn(134,"mail",     "deliver_maildir");
        // crypto:: (135-140)
        wire_module_fn(135,"crypto",   "sha256");
        wire_module_fn(136,"crypto",   "sha512");
        wire_module_fn(137,"crypto",   "md5");
        wire_module_fn(138,"crypto",   "hmac");
        wire_module_fn(139,"crypto",   "random_bytes");
        wire_module_fn(140,"crypto",   "random_hex");
        // base64:: (141-142)
        wire_module_fn(141,"base64",   "encode");
        wire_module_fn(142,"base64",   "decode");
        // uuid:: (143)
        wire_module_fn(143,"uuid",     "v4");
        // parallel:: (144-147)
        wire_module_fn(144,"parallel", "active");
        wire_module_fn(145,"parallel", "wait");
        wire_module_fn(146,"parallel", "limit");
        wire_module_fn(147,"parallel", "at_capacity");

        look::acquire_thread_connections();
        t_dispatch_start = std::chrono::steady_clock::now();
        bool vm_ok = false;
        try {
            look::VM::SharedState sh;
            sh.routes   = &route_closures;
            sh.builtins = &req_builtins;
            look::VM vm(sh, output);
            // Setup globals'ı yükle: db_check/json_ok gibi named fn'ler + $conn pool key
            vm.set_globals(g_http_app.vm_setup_globals);
            vm.set_web_context(&web);
            vm.dispatch_routes(web.method, web.path);
            vm_ok = true;
        } catch (const std::exception& vm_e) {
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                std::string("VM hata, interpreter fallback: ") + vm_e.what());
        }

        if (!vm_ok) {
            // Fallback — interpreter
            output.str(""); output.clear();
            web = make_web_ctx(req);
            copy->set_web_context(&web);
            try {
                copy->dispatch_routes();
            } catch (const std::exception& e) {
                if (web.status_code == 200) {
                    web.status_code = 500;
                    web.status_text = "Internal Server Error";
                }
                look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                    std::string("Fallback dispatch hatası: ") + e.what());
            }
        }
    } else {
        // ── Interpreter path (eski davranış) ──────────────────────────────────
        t_copy_start = std::chrono::steady_clock::now();
        auto copy = g_http_app.interp->make_dispatch_copy();
        t_copy_end = std::chrono::steady_clock::now();

        copy->set_output(output);
        copy->set_web_context(&web);
        sl.unlock();

        look::acquire_thread_connections();
        t_dispatch_start = std::chrono::steady_clock::now();
        try {
            bool is_direct = (copy->get_route_count() == 0);
            if (is_direct) {
                std::shared_lock<std::shared_mutex> sl2(g_http_mutex);
                copy->interpret(*g_http_app.program);
            } else {
                copy->dispatch_routes();
            }
        } catch (const std::exception& e) {
            if (web.status_code == 200) {
                web.status_code = 500;
                web.status_text = "Internal Server Error";
            }
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                std::string("Dispatch hatası: ") + e.what());
        }
    } // end interpreter path

    look::release_thread_connections();
    auto t_dispatch_end = std::chrono::steady_clock::now();

    // İlk 200 istekte zamanlama logla — profiling için
    static std::atomic<int> prof_count{0};
    int n = ++prof_count;
    if (n <= 200) {
        auto us_copy     = std::chrono::duration_cast<std::chrono::microseconds>(t_copy_end - t_copy_start).count();
        auto us_dispatch = std::chrono::duration_cast<std::chrono::microseconds>(t_dispatch_end - t_dispatch_start).count();
        if (n % 50 == 0) {
            look::Logger::instance().log(look::LogLevel::LOG_INFO, "PROF",
                "copy=" + std::to_string(us_copy) + "us dispatch=" + std::to_string(us_dispatch) + "us path=" + req.path);
        }
    }

    // Build response
    resp.status_code = web.status_code;
    resp.status_text = web.status_text;
    for (auto& [k, v] : web.headers_out) resp.headers[k] = v;
    resp.body = output.str();

    // Content-Type default
    if (resp.headers.find("Content-Type") == resp.headers.end())
        resp.headers["Content-Type"] = "text/html; charset=utf-8";
}

// ── Public entry point — called from fcgi_main.cpp when --mode http ───────────

void run_http_mode(int port, int workers, const std::string& script_path_str) {
#ifndef _WIN32
    // Client erken bağlantıyı kapatırsa write() SIGPIPE gönderir — varsayılan davranış process'i sonlandırır
    signal(SIGPIPE, SIG_IGN);
#endif

    // Working directory — same logic as fcgi mode
    fs::path script = fs::absolute(script_path_str);
    if (!fs::exists(script)) {
        std::cerr << "[look-fcgi] Dosya bulunamadı: " << script << "\n";
        return;
    }
    auto dir = script.parent_path().string();
#ifdef _WIN32
    (void)_chdir(dir.c_str());
#else
    (void)chdir(dir.c_str());
#endif

    look::set_db_pool_size(workers);

    // Initial setup
    try {
        std::unique_lock<std::shared_mutex> ul(g_http_mutex);
        run_setup_http(script);
    } catch (const std::exception& e) {
        std::cerr << "[look-fcgi --mode http] Setup hatası: " << e.what() << "\n";
        return;
    }

    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
        "look-fcgi --mode http port=" + std::to_string(port) + " workers=" + std::to_string(workers));
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Script: " + script.string());
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Apache bypass — nginx veya doğrudan bağlantı kullanın.");

    // WebSocket handler — called on a worker thread after 101 upgrade
    auto ws_handler = [](std::shared_ptr<look::WsConnection> conn,
                         const look::HttpRequest& req) {
        std::shared_lock<std::shared_mutex> sl(g_http_mutex);
        auto copy = g_http_app.interp->make_dispatch_copy();
        sl.unlock();

        look::WebContext web;
        web.method = "WS";
        web.path   = req.path;
        web.query_string = req.query_string;
        web.get_params   = look::WebContext::parse_query(req.query_string);
        auto iip = req.headers.find("x-forwarded-for");
        if (iip != req.headers.end()) web.remote_addr = iip->second;

        std::ostringstream sink;
        copy->set_output(sink);
        copy->set_web_context(&web);
        copy->set_ws_connection(conn);

        look::acquire_thread_connections();
        try {
            copy->dispatch_routes();
        } catch (const std::exception& e) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                std::string("WS dispatch hatası: ") + e.what());
        }
        look::release_thread_connections();
    };

    // SSE handler — called on a worker thread after SSE upgrade
    auto sse_handler = [](std::shared_ptr<look::SseConnection> conn,
                          const look::HttpRequest& req) {
        std::shared_lock<std::shared_mutex> sl(g_http_mutex);
        auto copy = g_http_app.interp->make_dispatch_copy();
        sl.unlock();

        look::WebContext web;
        web.method = "SSE";
        web.path   = req.path;
        web.query_string = req.query_string;
        web.get_params   = look::WebContext::parse_query(req.query_string);
        auto iip = req.headers.find("x-forwarded-for");
        if (iip != req.headers.end()) web.remote_addr = iip->second;

        std::ostringstream sink;
        copy->set_output(sink);
        copy->set_web_context(&web);
        copy->set_sse_connection(conn);

        look::acquire_thread_connections();
        try {
            copy->dispatch_routes();
        } catch (const std::exception& e) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                std::string("SSE dispatch hatası: ") + e.what());
        }
        look::release_thread_connections();
    };

    try {
        look::HttpServer server(port, workers, http_handler, ws_handler, sse_handler);
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
            "Listening on port " + std::to_string(port));
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[look-fcgi --mode http] " << e.what() << "\n";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", e.what());
    }
}

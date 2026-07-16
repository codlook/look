// look-fcgi --mode http
//
// HTTP/1.1 modunda doğrudan TCP dinler — Apache/FastCGI bypass.
// Nginx opsiyonel reverse proxy olarak önünde durabilir.
//
// Mevcut --mode fcgi (default) hiç değişmez.
// Aynı .lk kodu her iki modda çalışır.

#include "look/builtins.h"
#include "look/http_server.h"
#include <algorithm>
#include "look/db_async_pool.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/sse.h"
#include "look/smtp_server.h"
#include "look/imap_server.h"
#include "look/fiber.h"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <atomic>
#include <shared_mutex>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <thread>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <unistd.h>
#  include <signal.h>
#endif

namespace fs = std::filesystem;

// ── HTTP rate limiter (token bucket, per-IP + global) ────────────────────────
// LOOK_RATE_LIMIT_RPM        per-IP  dakika limiti     (0 = devre dışı)
// LOOK_RATE_LIMIT_BURST      per-IP  anlık burst        (varsayılan = RPM)
// LOOK_RATE_LIMIT_GLOBAL_RPM global  dakika limiti     (0 = devre dışı)
// LOOK_RATE_LIMIT_GLOBAL_BURST global anlık burst      (varsayılan = GLOBAL_RPM)
//
// Önce global bucket kontrol edilir; dolu ise per-IP'ye bakılmaz (CPU tasarrufu).
// On limit: returns true (caller should send 429).

static int rl_env(const char* name) {
    const char* e = std::getenv(name);
    if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
    return 0;
}

struct RlConfig {
    int per_rpm;
    int per_burst;
    int global_rpm;
    int global_burst;
};

static const RlConfig& rl_config() {
    static RlConfig c = []() -> RlConfig {
        int per_rpm   = rl_env("LOOK_RATE_LIMIT_RPM");
        int per_burst = rl_env("LOOK_RATE_LIMIT_BURST");
        if (per_burst <= 0) per_burst = per_rpm;
        int g_rpm     = rl_env("LOOK_RATE_LIMIT_GLOBAL_RPM");
        int g_burst   = rl_env("LOOK_RATE_LIMIT_GLOBAL_BURST");
        if (g_burst <= 0) g_burst = g_rpm;
        return {per_rpm, per_burst, g_rpm, g_burst};
    }();
    return c;
}

// Token bucket — fixed-point (tokens * 1000) for lock-free atomic ops.
// refill_per_ms = rpm / 60.0 tokens per ms (stored * 1000 as int64).
struct TokenBucket {
    std::atomic<int64_t> tokens{0};   // fixed-point * 1000
    std::atomic<int64_t> last_ms{0};
    int64_t max_tokens{0};            // burst * 1000
    int64_t refill_per_ms{0};         // rpm/60 * 1000

    void init(int rpm, int burst) {
        max_tokens    = (int64_t)burst * 1000;
        // rpm / 60 = tokens/second / 1000 * 1000 = token-units per ms
        // RPM=120 → 2 token-units/ms (1 request = 1000 units → 500ms to earn 1 token)
        refill_per_ms = std::max((int64_t)1, (int64_t)rpm / 60);
        tokens.store(max_tokens);
        last_ms.store(now_ms());
    }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void refill() {
        int64_t now  = now_ms();
        int64_t last = last_ms.load(std::memory_order_relaxed);
        int64_t elapsed = now - last;
        if (elapsed <= 0) return;
        if (last_ms.compare_exchange_weak(last, now,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            int64_t add = elapsed * refill_per_ms;
            int64_t cur = tokens.load(std::memory_order_relaxed);
            int64_t next = std::min(max_tokens, cur + add);
            tokens.store(next, std::memory_order_relaxed);
        }
    }

    // Returns true if request is allowed (consumed 1 token).
    bool consume() {
        if (max_tokens <= 0) return true; // disabled
        refill();
        int64_t cur = tokens.load(std::memory_order_relaxed);
        while (cur >= 1000) {
            if (tokens.compare_exchange_weak(cur, cur - 1000,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return true;
        }
        return false;
    }
};

// Global bucket — single shared bucket, lock-free.
static TokenBucket g_global_bucket;

// Per-IP rate limiter using token buckets.
// ~40 bytes per IP (vs ~120*sizeof(time_point) with sliding window).
struct RateLimiter {
    struct IpBucket {
        TokenBucket bucket;
        std::chrono::steady_clock::time_point last_seen;
    };
    std::unordered_map<std::string, IpBucket> states;
    std::mutex mtx;

    // Returns true if the IP should be rejected (rate limited).
    bool check(const std::string& ip, int rpm, int burst) {
        if (rpm <= 0) return false;
        std::lock_guard<std::mutex> lk(mtx);

        // Evict stale IPs (no hit in last 2 minutes) when map is large.
        if (states.size() > 50000) {
            auto now = std::chrono::steady_clock::now();
            auto stale = std::chrono::seconds(120);
            for (auto it = states.begin(); it != states.end(); )
                it = (now - it->second.last_seen > stale) ? states.erase(it) : ++it;
        }

        auto it = states.find(ip);
        if (it == states.end()) {
            auto& b = states[ip];
            b.bucket.init(rpm, burst);
            b.last_seen = std::chrono::steady_clock::now();
            return false; // first hit always allowed
        }
        it->second.last_seen = std::chrono::steady_clock::now();
        return !it->second.bucket.consume();
    }
};

static RateLimiter g_rate_limiter;

// ── Trusted proxy check for rate limiter ─────────────────────────────────────
// LOOK_TRUSTED_PROXY=127.0.0.1,10.0.0.0/8  (comma-separated IPs or /prefix CIDRs)
// When set: only accept X-Real-IP / X-Forwarded-For from these addresses.
// When unset: always use req.remote_addr (the real TCP socket IP).
static bool is_trusted_proxy(const std::string& ip) {
    static const std::string raw = []() -> std::string {
        const char* e = std::getenv("LOOK_TRUSTED_PROXY");
        return e ? e : "";
    }();
    if (raw.empty()) return false;
    // Parse comma-separated list on first call
    static const std::vector<std::string> proxies = []() {
        std::vector<std::string> v;
        std::istringstream ss(raw);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            tok.erase(0, tok.find_first_not_of(" \t"));
            tok.erase(tok.find_last_not_of(" \t") + 1);
            if (!tok.empty()) v.push_back(tok);
        }
        return v;
    }();
    for (const auto& entry : proxies) {
        auto slash = entry.find('/');
        std::string base = (slash != std::string::npos) ? entry.substr(0, slash) : entry;
        if (slash == std::string::npos) { if (ip == base) return true; continue; }
        int prefix = 0;
        try { prefix = std::stoi(entry.substr(slash + 1)); } catch (...) { continue; }  // bozuk CIDR → atla
        if (prefix < 0 || prefix > 32) continue;
        uint32_t mask = prefix ? (~0u << (32 - prefix)) : 0;
        auto to_int = [](const std::string& s) -> uint32_t {
            uint32_t r = 0;
            std::istringstream ss2(s); std::string seg;
            while (std::getline(ss2, seg, '.')) {
                try { r = (r << 8) | (std::stoul(seg) & 0xFF); } catch (...) { return 0; }
            }
            return r;
        };
        if ((to_int(ip) & mask) == (to_int(base) & mask)) return true;
    }
    return false;
}

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
    struct VmRouteRaw { std::string pattern; look::Value fn; std::vector<look::Value> middlewares; };
    std::vector<VmRouteRaw>                          vm_routes;
    // Route bazında kalıcı VM devre dışı bayrağı — VM'de bir kez hata veren
    // route sonraki isteklerde doğrudan interpreter'a gider (her istekte
    // dene→hata→fallback döngüsü yerine). vm_routes ile aynı indeks.
    // uint8_t: benign race (idempotent 1 yazımı), mutex gerekmez.
    std::vector<uint8_t>                     vm_route_disabled;
    // VM'nin dispatch için hazır olduğunu gösterir.
    bool                                     vm_routes_ready = false;
    // before_route() middleware listesi (setup fazında kaydedilir, dispatch'ten önce çalışır)
    std::vector<look::Value>                 vm_before_routes;
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
    g_http_app.vm_before_routes.clear();
    g_http_app.vm_routes_ready = false;
    if (g_http_app.use_bytecode && g_http_app.compiled) {
        try {
            // Setup builtins: route() (index 22) ve temel veri fonksiyonları
            // Diğer builtin'ler setup'ta çağrılmaz (db::query vb. HTTP context ister)
            auto vm_routes_ptr        = &g_http_app.vm_routes;
            auto vm_before_routes_ptr = &g_http_app.vm_before_routes;
            std::vector<look::BuiltinFn> setup_builtins(look::builtin_names().size());

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
                try { return look::Value((int64_t)std::stoll(args[0].to_string())); }
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

            // before_route() — setup'ta middleware closure'ı kaydet. İndeks İSİMDEN
            // (builtin_index) — hard-coded sayı builtin listesi büyüyünce kayıyordu.
            setup_builtins[(size_t)look::builtin_index("before_route")] =
                [vm_before_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                if (!args.empty() && args[0].type() == look::Value::BYTECODE_FN)
                    vm_before_routes_ptr->push_back(args[0]);
                return look::Value();
            };

            // stop() — setup'ta no-op (sadece dispatch'te anlamlı)
            setup_builtins[(size_t)look::builtin_index("stop")] =
                [](std::vector<look::Value>&) -> look::Value {
                return look::Value();
            };

            // db::connect (setup'ta çağrılır) — interpreter fallback ile
            // NOT: db:: fonksiyonları setup'ta interpreter tarafından çalıştırıldı.
            // VM setup fazında db:: yok — sadece route() kayıt önemli.

            // route() — index 22: pattern + closure kaydet
            setup_builtins[22] = [vm_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                // route("GET", "/path", fn) — 3-arg form
                // route("GET", "/path", [mw...], fn) — 4-arg form
                // route("404", fn) — 2-arg form
                if (args.size() >= 4) {
                    // 4-arg: method, pattern, middlewares, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = args[1].to_string();
                    std::vector<look::Value> middlewares;
                    if (args[2].type() == look::Value::ARRAY)
                        middlewares = *args[2].as_array();
                    if (args[3].type() == look::Value::BYTECODE_FN) {
                        vm_routes_ptr->push_back({method + ":" + pattern, args[3], std::move(middlewares)});
                    }
                } else if (args.size() >= 3) {
                    // 3-arg: method, pattern, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = args[1].to_string();
                    if (args[2].type() == look::Value::BYTECODE_FN) {
                        vm_routes_ptr->push_back({method + ":" + pattern, args[2], {}});
                    }
                } else if (args.size() >= 2) {
                    // 2-arg: "404" + fn
                    std::string method  = args[0].to_string();
                    if (args[1].type() == look::Value::BYTECODE_FN) {
                        vm_routes_ptr->push_back({method + ":/", args[1], {}});
                    }
                }
                return look::Value();
            };

            // db::connect (56) — interpreter'ın mevcut pool key'ini döndür (aynı DSN için yeni
            // pool açmaz). VM route closures bu key'i capture eder; acquire_thread_connections()
            // bu pool'dan bağlantı alır. Farklı DSN için yeni pool açmak gerekmez (index.lk tek DSN).
            // KRİTİK: bu override'lar artık İSİMDEN indeksleniyor (builtin_index).
            // Eskiden hard-coded sayılardı; builtin_names()'e giriş eklenince
            // (ör. session::regenerate) tüm indeksler kaydı ve db::connect setup'ta
            // channel lambda'sını çalıştırıyordu (sessiz VM/interpreter divergence).
            auto SBI = [](const char* n) { return (size_t)look::builtin_index(n); };
            setup_builtins[SBI("db::connect")] = [](std::vector<look::Value>& args) -> look::Value {
                look::Value existing = g_http_app.interp->get_global("conn");
                if (existing.type() == look::Value::STRING) return existing;
                auto f = g_http_app.interp->get_module_fn("db", "connect");
                if (!f) return look::Value();
                std::vector<look::Value> a = args;
                return f(a);
            };
            setup_builtins[SBI("channel")] = [](std::vector<look::Value>& args) -> look::Value {
                int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
                if (buf <= 0) buf = (1 << 20);
                return look::Value(std::make_shared<look::LookChannel>(buf));
            };
            setup_builtins[SBI("env")] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value();
                std::string key = args[0].to_string();
                std::string def = (args.size() >= 2) ? args[1].to_string() : "";
                return look::Value(look::look_get_env(key, def));
            };
            {
                auto f = g_http_app.interp->get_module_fn("", "config");
                if (f) setup_builtins[SBI("config")] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args;
                    return f(a);
                };
            }
            {
                auto f = g_http_app.interp->get_module_fn("date", "timestamp");
                if (f) setup_builtins[SBI("date::timestamp")] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args; return f(a);
                };
            }
            for (auto& [nm, mod, fn] : std::vector<std::tuple<std::string,std::string,std::string>>{
                {"file::read","file","read"},{"file::put","file","put"},{"file::append","file","append"},
                {"file::exists","file","exists"},{"file::remove","file","remove"},
                {"file::size","file","size"},{"file::store","file","store"}
            }) {
                auto f = g_http_app.interp->get_module_fn(mod, fn);
                if (f) setup_builtins[SBI(nm.c_str())] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args; return f(a);
                };
            }
            // app:: — servis kaydi setup'ta cagrilir (app::set("db", $conn)).
            // Index isimden alinir (builtin_index) — append-only, hizasizlik yok.
            for (auto& [name, mod, fn] : std::vector<std::tuple<std::string,std::string,std::string>>{
                {"app::set","app","set"},{"app::get","app","get"},
                {"app::has","app","has"},{"app::db","app","db"}
            }) {
                int idx = look::builtin_index(name);
                auto f = g_http_app.interp->get_module_fn(mod, fn);
                if (idx >= 0 && f) setup_builtins[idx] = [f](std::vector<look::Value>& args) -> look::Value {
                    std::vector<look::Value> a = args; return f(a);
                };
            }

            // ── Boş slot güvenlik ağı — VM'in sessizce devre dışı kalmasını önler ──
            // Setup fazında, yukarıda açıkça bağlanmamış herhangi bir builtin
            // çağrılırsa (örn. db::exec, crypto::uuid, json::encode), boş bir
            // std::function çağrılır → std::bad_function_call → catch → tüm VM
            // devre dışı → en yaygın kalıpta (setup'ta DB) VM sessizce kapanırdı.
            // Çözüm: kalan tüm slotları doldur.
            //   • Yan etkili modüller (db yazma, http, mail, queue, jobs, cache,
            //     ws, sse) → NO-OP: interpreter setup'ı (satır ~285) bu yan
            //     etkileri ZATEN çalıştırdı; VM tekrarında ikinci kez çalışması
            //     çift INSERT / çift istek / "table exists" hatası yapardı.
            //   • Saf fonksiyonlar (string, json, crypto, math, date…) →
            //     interpreter'a fallback (gerçek değer döner; dinamik route
            //     path'i bunlara dayanırsa VM route'u interpreter'la eşleşir).
            {
                const auto& bnames = look::builtin_names();
                auto starts_with = [](const std::string& s, const char* p) {
                    return s.rfind(p, 0) == 0;
                };
                auto is_side_effect = [&](const std::string& n) {
                    // db::connect hariç db:: yan etkilidir (setup'ta zaten çalıştı)
                    if (starts_with(n, "db::")) return n != "db::connect";
                    return starts_with(n, "http::") || starts_with(n, "mail::") ||
                           starts_with(n, "queue::") || starts_with(n, "jobs::") ||
                           starts_with(n, "cache::") || starts_with(n, "ws::") ||
                           starts_with(n, "sse::");
                };
                for (size_t i = 0; i < setup_builtins.size() && i < bnames.size(); ++i) {
                    if (setup_builtins[i]) continue;           // zaten bağlı
                    const std::string& name = bnames[i];
                    if (is_side_effect(name)) {
                        setup_builtins[i] = [](std::vector<look::Value>&) -> look::Value {
                            return look::Value();              // setup'ta no-op
                        };
                        continue;
                    }
                    auto colon = name.find("::");
                    if (colon != std::string::npos) {
                        std::string mod = name.substr(0, colon);
                        std::string fn  = name.substr(colon + 2);
                        auto f = g_http_app.interp->get_module_fn(mod, fn);
                        if (f) {
                            setup_builtins[i] = [f](std::vector<look::Value>& args) -> look::Value {
                                std::vector<look::Value> a = args;
                                return f(a);
                            };
                            continue;
                        }
                    }
                    // Modülsüz veya bulunamayan builtin → güvenli no-op (crash yerine)
                    setup_builtins[i] = [](std::vector<look::Value>&) -> look::Value {
                        return look::Value();
                    };
                }
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
                g_http_app.vm_route_disabled.assign(g_http_app.vm_routes.size(), 0);
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

// ── Paylaşılan uygulama motoru API'si ─────────────────────────────────────────
// FCGI modu (fcgi_main.cpp) ile HTTP modu aynı setup + dispatch motorunu
// paylaşır — VM dispatch TEK yerde yaşar. (Borç 5: FCGI interpreter'a
// mahkumdu; artık her iki mod da bytecode VM kullanır.)
//
// NOT: motor tek script tutar. FCGI'da SCRIPT_FILENAME istekten isteğe
// değişirse her değişimde tam re-setup olur — servis-başına-tek-app
// kurulumunda (production modeli) bu hiç tetiklenmez.

bool look_http_engine_ensure(const std::string& script_path_str, std::string& err) {
    fs::path script = fs::absolute(script_path_str);
    fs::file_time_type mtime{};
    try { mtime = fs::last_write_time(script); } catch (...) {}

    {
        std::shared_lock<std::shared_mutex> sl(g_http_mutex);
        if (g_http_ready && g_http_app.script_path == script && g_http_app.mtime == mtime)
            return true;
    }
    std::unique_lock<std::shared_mutex> ul(g_http_mutex);
    if (g_http_ready && g_http_app.script_path == script && g_http_app.mtime == mtime)
        return true;
    try {
        run_setup_http(script);
        return true;
    } catch (const std::exception& e) {
        g_http_ready = false;
        err = e.what();
        return false;
    }
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

    // All request headers (already lowercase from HTTP parser)
    ctx.headers_in = req.headers;

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

// İleri bildirim — tanım http_handler'dan sonra (paylaşılan motor)
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path);

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
        // Use real TCP peer IP from socket.  Only trust proxy headers (X-Real-IP /
        // X-Forwarded-For) when the connecting IP is in LOOK_TRUSTED_PROXY list.
        std::string client_ip = req.remote_addr;
        if (is_trusted_proxy(req.remote_addr)) {
            auto ireal = req.headers.find("x-real-ip");
            if (ireal != req.headers.end()) {
                client_ip = ireal->second;
            } else {
                auto ixff = req.headers.find("x-forwarded-for");
                if (ixff != req.headers.end()) {
                    client_ip = ixff->second;
                    auto comma = client_ip.find(',');
                    if (comma != std::string::npos) client_ip = client_ip.substr(0, comma);
                }
            }
        }
        const auto& rlc = rl_config();

        // Global limit — önce kontrol et, dolu ise per-IP'ye bakma.
        if (rlc.global_rpm > 0) {
            static std::once_flag g_init;
            std::call_once(g_init, [&]() {
                g_global_bucket.init(rlc.global_rpm, rlc.global_burst);
            });
            if (!g_global_bucket.consume()) {
                resp.status_code = 429;
                resp.status_text = "Too Many Requests";
                resp.headers["Content-Type"] = "text/plain";
                resp.headers["Retry-After"]  = "1";
                resp.body = "429 Too Many Requests";
                return;
            }
        }

        // Per-IP limit.
        if (!client_ip.empty() && rlc.per_rpm > 0 &&
            g_rate_limiter.check(client_ip, rlc.per_rpm, rlc.per_burst)) {
            resp.status_code = 429;
            resp.status_text = "Too Many Requests";
            resp.headers["Content-Type"] = "text/plain";
            resp.headers["Retry-After"]  = "60";
            resp.body = "429 Too Many Requests";
            return;
        }
    }

    // Built-in health endpoint — rate limit bypass, no LOOK context needed
    {
        static std::string health_path = []() -> std::string {
            const char* e = std::getenv("LOOK_HEALTH_PATH");
            return e ? e : "/health";
        }();
        if (req.method == "GET" && req.path == health_path) {
            resp.status_code = 200;
            resp.status_text = "OK";
            resp.headers["Content-Type"] = "text/plain";
            resp.body = "ok";
            return;
        }
    }

    // Dispatch — paylaşılan motor (FCGI modu da aynı fonksiyonu kullanır)
    look::WebContext web = make_web_ctx(req);
    std::ostringstream output;
    look_app_dispatch(web, output, req.path);

    // Build response
    resp.status_code = web.status_code;
    resp.status_text = web.status_text;
    for (auto& [k, v] : web.headers_out) resp.headers[k] = v;
    resp.set_cookies = web.set_cookies_out;   // her çerez ayrı Set-Cookie satırı
    resp.body = web.response_body.empty() ? output.str() : web.response_body;

    // Content-Type default
    if (resp.headers.find("Content-Type") == resp.headers.end())
        resp.headers["Content-Type"] = "text/html; charset=utf-8";
}

// Paylaşılan istek motoru — web ctx hazırlanmış olmalı; VM (varsa) veya
// interpreter ile dispatch eder, çıktıyı output/web.response_body'ye yazar.
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path) {
    // Fallback için bozulmamış kopya (VM kısmi header/status yazmış olabilir)
    const look::WebContext web0 = web;

    std::shared_lock<std::shared_mutex> sl(g_http_mutex);
    // vm_routes_ready: VM'nin dispatch_routes() kullanabilmesi için
    // native function wiring tamamlanmış olmalı (Phase 16.5 Adım 5.5)
    bool use_bc = g_http_app.use_bytecode && g_http_app.compiled && g_http_app.vm_routes_ready;

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
        std::vector<look::VmRoute> route_closures;
        route_closures.reserve(g_http_app.vm_routes.size());
        for (size_t ri = 0; ri < g_http_app.vm_routes.size(); ++ri) {
            auto& r = g_http_app.vm_routes[ri];
            if (r.fn.type() != look::Value::BYTECODE_FN) continue;
            // app_index: hata durumunda route'u kalıcı interpreter'a sabitlemek
            // için g_http_app.vm_route_disabled ile eşleşen kalıcı indeks.
            // Sabitli route listede KALIR — dispatch eşleştirir, VmRouteDisabled
            // fırlatır, caller sessizce interpreter'a düşer (404'e düşmesin diye).
            look::VmRoute vr;
            vr.pattern   = r.pattern;
            vr.fn        = r.fn.as_bytecode_fn().get();
            vr.app_index = (int)ri;
            for (auto& mw : r.middlewares)
                if (mw.type() == look::Value::BYTECODE_FN)
                    vr.middlewares.push_back(mw.as_bytecode_fn().get());
            route_closures.push_back(std::move(vr));
        }
        // Statik route'ları ({param} içermeyenler) dinamiklerden ÖNCE sırala:
        // /user/new, /user/{id}'den önce eşleşsin (kayıt sırasından bağımsız,
        // deterministik routing). stable_partition grup-içi sırayı korur.
        std::stable_partition(route_closures.begin(), route_closures.end(),
            [](const look::VmRoute& r){ return r.pattern.find('{') == std::string::npos; });
        sl.unlock();

        // Per-request builtins: module fonksiyonları interpreter copy'den alınır
        // (copy->set_web_context(&web) çağrıldığında modules_ yeni web'e bind edildi)
        std::vector<look::BuiltinFn> req_builtins(look::builtin_names().size());

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

        // ── Modül builtin'leri: TEK kaynaktan otomatik wiring ────────────────
        // builtin_names() (look/builtins.h) içindeki her "mod::fn" ismi
        // interpreter'ın modül fonksiyonuna bağlanır. Yeni "mod::fn" builtin'i
        // eklemek için SADECE builtins.cpp listesine eklemek yeterli —
        // elle senkron yok (strlen bug'ının kökü elle senkron kopukluğuydu).
        {
            const auto& names = look::builtin_names();
            for (size_t idx = 0; idx < names.size(); ++idx) {
                auto pos = names[idx].find("::");
                if (pos == std::string::npos) continue;  // özel isim — elle bağlanır
                auto f = copy->get_module_fn(names[idx].substr(0, pos),
                                             names[idx].substr(pos + 2));
                if (f) {
                    req_builtins[idx] = [f](std::vector<look::Value>& args) -> look::Value {
                        std::vector<look::Value> a = args;
                        return f(a);
                    };
                }
            }
        }

        // KRİTİK: manuel override indeksleri artık İSİMDEN çözülüyor (builtin_index).
        // Eskiden hard-coded sayılardı; builtin_names() listesine bir giriş eklenince
        // tüm indeksler kayıyor ve bu override'lar YANLIŞ builtin'i eziyordu
        // (ör. before_route no-op'u response::json'ı ezip JSON API'lerini boş
        // döndürüyordu). İsim-tabanlı çözüm bu senkron-kopukluğu sınıfını kapatır.
        auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };

        // channel()
        req_builtins[BI("channel")] = [](std::vector<look::Value>& args) -> look::Value {
            int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
            if (buf <= 0) buf = (1 << 20);
            return look::Value(std::make_shared<look::LookChannel>(buf));
        };
        // env() ve config()
        req_builtins[BI("env")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value();
            std::string key = args[0].to_string();
            std::string def = (args.size() >= 2) ? args[1].to_string() : "";
            return look::Value(look::look_get_env(key, def));
        };
        {
            auto f_cfg = copy->get_module_fn("", "config");
            if (f_cfg) req_builtins[BI("config")] = [f_cfg](std::vector<look::Value>& args) -> look::Value {
                std::vector<look::Value> a = args; return f_cfg(a);
            };
        }

        // before_route() — dispatch'te no-op (setup'ta kaydedildi)
        req_builtins[BI("before_route")] = [](std::vector<look::Value>&) -> look::Value { return look::Value(); };

        // stop() — before_route middleware içinden route execution'ı iptal et
        req_builtins[BI("stop")] = [](std::vector<look::Value>&) -> look::Value {
            throw look::RouteStopException();
        };

        // exit()/die() — interpreter ile aynı semantik (ExitException). Bağlanmazsa
        // builtin slot'u boş kalır → std::bad_function_call → route KALICI interpreter'a
        // düşerdi. Web'de exit() zaten nadir; davranış interpreter yolundakiyle aynı.
        {
            auto do_exit = [](std::vector<look::Value>& a) -> look::Value {
                int code = 0;
                if (!a.empty() && a[0].type() == look::Value::INT) code = (int)a[0].as_int();
                throw look::ExitException(code);
            };
            req_builtins[BI("exit")] = do_exit;
            req_builtins[BI("die")]  = do_exit;
        }

        // skaler core builtins — interpreter ile birebir
        req_builtins[BI("strlen")] = [](std::vector<look::Value>& args) -> look::Value {
            return look::Value(args.empty() ? 0 : (int)args[0].to_string().size());
        };
        req_builtins[BI("abs")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0);
            auto& v = args[0];
            if (v.type() == look::Value::FLOAT) return look::Value(std::abs(v.as_float()));
            return look::Value(std::abs(v.to_int()));
        };
        req_builtins[BI("max")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.size() < 2) return args.empty() ? look::Value() : args[0];
            return args[0] >= args[1] ? args[0] : args[1];
        };
        req_builtins[BI("min")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.size() < 2) return args.empty() ? look::Value() : args[0];
            return args[0] <= args[1] ? args[0] : args[1];
        };
        req_builtins[BI("sqrt")] = [](std::vector<look::Value>& args) -> look::Value {
            return look::Value(args.empty() ? 0.0 : std::sqrt(args[0].to_float()));
        };
        req_builtins[BI("strtoupper")] = [](std::vector<look::Value>& args) -> look::Value {
            std::string s = args.empty() ? "" : args[0].to_string();
            for (char& c : s) c = (char)std::toupper((unsigned char)c);
            return look::Value(s);
        };
        req_builtins[BI("strtolower")] = [](std::vector<look::Value>& args) -> look::Value {
            std::string s = args.empty() ? "" : args[0].to_string();
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return look::Value(s);
        };
        // push/pop: builtin_names'de vardı ama req_builtins'de bağlı DEĞİLDİ →
        // route içinde bare push/pop "bad function call" fırlatıp interpreter'a
        // düşürüyordu. In-place mutasyon: args[0] çağıranın $arr'ıyla shared_ptr
        // paylaşır (kopya olsa da aynı vector) → $arr yerinde büyür/küçülür.
        req_builtins[BI("push")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("push() requires array and value");
            args[0].as_array()->push_back(args[1]);
            return args[0];
        };
        req_builtins[BI("pop")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty() || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("pop() requires array");
            auto a = args[0].as_array();
            if (a->empty()) return look::Value();
            look::Value last = a->back();
            a->pop_back();
            return last;
        };
        // bare join(arr, sep="") — interpreter global builtin'i (fn_name=="join").
        req_builtins[BI("join")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty() || args[0].type() != look::Value::ARRAY)
                return look::Value(args.empty() ? std::string() : args[0].to_string());
            std::string sep = args.size() >= 2 ? args[1].to_string() : "";
            std::string result;
            auto& arr = *args[0].as_array();
            for (size_t i = 0; i < arr.size(); ++i) { if (i) result += sep; result += arr[i].to_string(); }
            return look::Value(result);
        };

        // before_route closures: route_closures ile aynı yapıda
        std::vector<look::Closure*> before_closures;
        before_closures.reserve(g_http_app.vm_before_routes.size());
        for (auto& val : g_http_app.vm_before_routes)
            if (val.type() == look::Value::BYTECODE_FN)
                before_closures.push_back(val.as_bytecode_fn().get());

        look::acquire_thread_connections();
        t_dispatch_start = std::chrono::steady_clock::now();
        bool vm_ok = false;
        int  vm_failed_route = -1;
        try {
            look::VM::SharedState sh;
            sh.routes         = &route_closures;
            sh.builtins       = &req_builtins;
            sh.route_disabled = &g_http_app.vm_route_disabled;
            look::VM vm(sh, output);
            vm.set_globals(g_http_app.vm_setup_globals);
            vm.set_web_context(&web);

            // before_route middleware'leri sırayla çalıştır
            // stop() throw ettiğinde RouteStopException yakalanır → route atlanır
            bool stopped = false;
            for (auto* cl : before_closures) {
                try {
                    vm.call_closure(*cl, {});
                } catch (const look::RouteStopException&) {
                    stopped = true;
                    break;
                }
            }

            if (!stopped) {
                try {
                    vm.dispatch_routes(web.method, web.path);
                } catch (const look::VmRouteDisabled&) {
                    // Route kalıcı interpreter'a sabitli — sessiz fallback, log yok
                    throw;
                } catch (const look::RouteStopException&) {
                    throw;
                } catch (const look::ExitException&) {
                    throw;   // exit()/die() — normal akış, VM hatası DEĞİL (route'u suçlama)
                } catch (...) {
                    vm_failed_route = vm.last_matched_route();
                    throw;
                }
            }
            vm_ok = true;
        } catch (const look::RouteStopException&) {
            vm_ok = true;  // stop() route handler içinde çağrıldı — normal akış
        } catch (const look::ExitException&) {
            // exit()/die() — script'i sonlandırır, VM hatası DEĞİL. Genel catch'e
            // düşerse route KALICI interpreter'a sabitlenirdi (yanlış suçlama).
            // O ana dek yazılmış çıktı yanıt olur — interpreter semantiğiyle aynı.
            vm_ok = true;
        } catch (const look::VmRouteDisabled&) {
            // beklenen akış — interpreter fallback'e düş (aşağıda)
        } catch (const std::exception& vm_e) {
            // İlk hata: route'u kalıcı interpreter'a sabitle — bu route için
            // her istekte dene→hata→fallback döngüsü bir daha yaşanmaz.
            if (vm_failed_route >= 0 &&
                vm_failed_route < (int)g_http_app.vm_route_disabled.size() &&
                !g_http_app.vm_route_disabled[vm_failed_route]) {
                g_http_app.vm_route_disabled[vm_failed_route] = 1;
                look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                    "Route VM'den çıkarıldı (kalıcı interpreter): " +
                    g_http_app.vm_routes[vm_failed_route].pattern + " — " + vm_e.what());
            } else {
                look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                    std::string("VM hata, interpreter fallback: ") + vm_e.what());
            }
        }

        if (!vm_ok) {
            // Fallback — interpreter (web'i bozulmamış kopyadan sıfırla)
            output.str(""); output.clear();
            web = web0;
            copy->set_web_context(&web);
            try {
                copy->dispatch_routes();
            } catch (const look::RouteStopException&) {
                // stop() middleware'de çağrıldı — normal akış, response hazır
            } catch (const std::exception& e) {
                if (web.status_code == 200) {
                    web.status_code = 500;
                    web.status_text = "Internal Server Error";
                }
                look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                    std::string("Fallback dispatch hatası: ") + e.what());
            }
        }
        // VM path acquire (satır 937) burada serbest bırakılır — vm_ok ve
        // fallback dahil her iki akışı kapsar. Eksik release, pool_size istek
        // sonrası tüm worker'ların deadlock olmasına neden oluyordu.
        look::release_thread_connections();
    } else {
        // ── Interpreter path ──────────────────────────────────────────────────
        t_copy_start = std::chrono::steady_clock::now();
        auto copy = g_http_app.interp->make_dispatch_copy();
        t_copy_end = std::chrono::steady_clock::now();

        copy->set_output(output);
        copy->set_web_context(&web);
        sl.unlock();

        t_dispatch_start = std::chrono::steady_clock::now();

        // ── Fiber dispatch (LOOK_FIBER_DISPATCH=1 ile aktif) ──────────────────
        // Fiber path: connections are acquired lazily inside the fiber via
        // get_conn() fallback → pinned to fiber->local.conns.
        // Thread-level acquire_thread_connections() is intentionally skipped:
        // it would fill tl_conns (not fiber->local.conns), causing pool exhaustion
        // when the fiber tries to acquire its own connection (deadlock).
        static const bool use_fiber = []() {
            const char* v = std::getenv("LOOK_FIBER_DISPATCH");
            return v && std::string(v) == "1";
        }();

        if (use_fiber) {
            std::exception_ptr dispatch_ex;

            auto do_dispatch = [&copy, &web, &dispatch_ex]() {
                try {
                    bool is_direct = (copy->get_route_count() == 0);
                    if (is_direct) {
                        std::shared_lock<std::shared_mutex> sl2(g_http_mutex);
                        copy->interpret(*g_http_app.program);
                    } else {
                        copy->dispatch_routes();
                    }
                } catch (...) {
                    dispatch_ex = std::current_exception();
                }
                look::release_thread_connections();
            };

            if (look::Fiber::current() != nullptr) {
                // Already running inside a connection fiber (burst-accept path).
                // Execute directly — wait_readable() will use the outer tl_srv_sched.
                do_dispatch();
            } else {
                // Standalone path: spawn a fiber and run it to completion.
                static thread_local look::FiberScheduler tl_fiber_sched;
                static thread_local bool sched_initialized = false;
                if (!sched_initialized) {
                    look::set_thread_scheduler(&tl_fiber_sched);
                    sched_initialized = true;
                }
                tl_fiber_sched.spawn(do_dispatch);
                tl_fiber_sched.run_until_complete();
            }

            if (dispatch_ex) {
                try { std::rethrow_exception(dispatch_ex); }
                catch (const std::exception& e) {
                    if (web.status_code == 200) {
                        web.status_code = 500;
                        web.status_text = "Internal Server Error";
                    }
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                        std::string("Fiber dispatch hatası: ") + e.what());
                }
            }
        } else {
            // ── Blocking path (default) ───────────────────────────────────────
            look::acquire_thread_connections();
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
            look::release_thread_connections();
        }
    } // end interpreter path

    auto t_dispatch_end = std::chrono::steady_clock::now();

    // İlk 200 istekte zamanlama logla — profiling için
    static std::atomic<int> prof_count{0};
    int n = ++prof_count;
    if (n <= 200) {
        auto us_copy     = std::chrono::duration_cast<std::chrono::microseconds>(t_copy_end - t_copy_start).count();
        auto us_dispatch = std::chrono::duration_cast<std::chrono::microseconds>(t_dispatch_end - t_dispatch_start).count();
        if (n % 50 == 0) {
            look::Logger::instance().log(look::LogLevel::LOG_INFO, "PROF",
                "copy=" + std::to_string(us_copy) + "us dispatch=" + std::to_string(us_dispatch) + "us path=" + prof_path);
        }
    }
}

// ── Public entry point — called from fcgi_main.cpp when --mode http ───────────

void run_http_mode(int port, int workers, const std::string& script_path_str) {
#ifndef _WIN32
    // Client erken bağlantıyı kapatırsa write() SIGPIPE gönderir — varsayılan davranış process'i sonlandırır
    signal(SIGPIPE, SIG_IGN);
    // tz global state'ini worker thread'ler başlamadan bir kez doldur —
    // localtime_r içindeki tzset erişimini yarışsız kılar (TSan date race fix).
    tzset();
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

    {
        int pool_sz = workers;  // default: workers ile aynı (güvenli başlangıç)
        const char* env = std::getenv("LOOK_DB_POOL_SIZE");
        if (env && env[0]) pool_sz = std::atoi(env);
        pool_sz = std::max(1, std::min(pool_sz, 256));
        look::set_db_pool_size(pool_sz);

        // AsyncDbPool — DB işlerini HTTP worker'lardan ayıran thread pool
        // LOOK_DB_ASYNC_THREADS: varsayılan pool_sz, 0 ise devre dışı
        int async_threads = pool_sz;
        const char* ath_env = std::getenv("LOOK_DB_ASYNC_THREADS");
        if (ath_env && ath_env[0]) async_threads = std::atoi(ath_env);
        if (async_threads > 0)
            look::set_db_async_pool(std::make_unique<look::AsyncDbPool>(async_threads));
    }

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

#ifndef _WIN32   // Gömülü mail sunucuları POSIX-only (OpenSSL). Windows = XAMPP dev.
    // SMTP server — LOOK_SMTP_PORT env var set edilirse HTTP ile birlikte başlar
    std::unique_ptr<look::SmtpServer> smtp_srv;
    std::thread smtp_thread;
    const char* smtp_port_env = std::getenv("LOOK_SMTP_PORT");
    if (smtp_port_env && *smtp_port_env) {
        int smtp_port = std::atoi(smtp_port_env);
        int sub_port  = 0;
        if (const char* e = std::getenv("LOOK_SMTP_SUB_PORT")) sub_port = std::atoi(e);
        if (smtp_port > 0) {
            smtp_srv = std::make_unique<look::SmtpServer>(
                smtp_port, sub_port, 0, workers,
                [](const look::SmtpMessage& msg) -> bool {
                    // Varsayılan handler: her yerel alıcının Maildir'ine yaz.
                    // Yol: <base>/<alıcı>/inbox — IMAP LOGIN'in okuduğu yerle aynı
                    // (mail_user_auth ile hizalı). Alıcı adı e-posta = LOGIN kullanıcı adı.
                    const char* base = std::getenv("LOOK_MAIL_DIR");
                    if (!base || !*base) base = "/var/mail/look";
                    bool any = false;
                    for (const std::string& rcpt : msg.rcpt_to) {
                        // GÜVENLİK: alıcıyı dizin bileşeni yapmadan traversal ele
                        bool unsafe = rcpt.empty() || rcpt.find("..") != std::string::npos;
                        for (unsigned char c : rcpt)
                            if (c == '/' || c == '\\' || c == '\0' || c < 0x20) unsafe = true;
                        if (unsafe) {
                            look::Logger::instance().log(look::LogLevel::LOG_WARN,
                                "smtp", "güvensiz alıcı adı atlandı: " + rcpt);
                            continue;
                        }
                        try {
                            look::deliver_maildir(std::string(base) + "/" + rcpt, "inbox", msg);
                            any = true;
                        } catch (const std::exception& e) {
                            look::Logger::instance().log(look::LogLevel::LOG_ERROR,
                                "smtp", std::string("deliver_maildir: ") + e.what());
                        }
                    }
                    return any;
                }
            );
            smtp_thread = std::thread([&smtp_srv] { smtp_srv->run(); });
            look::Logger::instance().log(look::LogLevel::LOG_INFO, "smtp",
                std::string("SMTP server started on port ") + smtp_port_env);
        }
    }

    // IMAP server — LOOK_IMAP_PORT set edilirse HTTP+SMTP ile birlikte başlar.
    // Kimlik: mail_user_auth (SMTP AUTH ile aynı model). INBOX = SMTP'nin yazdığı
    // <LOOK_MAIL_DIR>/<user>/inbox. STARTTLS/IMAPS için LOOK_IMAP_CERT/KEY.
    std::unique_ptr<look::ImapServer> imap_srv;
    const char* imap_port_env = std::getenv("LOOK_IMAP_PORT");
    if (imap_port_env && *imap_port_env) {
        int imap_port = std::atoi(imap_port_env);
        int imaps_port = 0;
        if (const char* e = std::getenv("LOOK_IMAP_PORT_TLS")) imaps_port = std::atoi(e);
        if (imap_port > 0) {
            imap_srv = std::make_unique<look::ImapServer>(
                imap_port, imaps_port, workers,
                [](const std::string& user, const std::string& pass) -> look::ImapAuthResult {
                    look::ImapAuthResult r;
                    std::string maildir;
                    if (look::mail_user_auth(user, pass, maildir)) {
                        r.ok = true; r.maildir_path = maildir;
                    }
                    return r;
                });
            imap_srv->start();  // kendi accept thread'lerini yönetir
            look::Logger::instance().log(look::LogLevel::LOG_INFO, "imap",
                std::string("IMAP server started on port ") + imap_port_env);
        }
    }
#endif  // _WIN32 — mail sunucuları

    try {
        look::HttpServer server(port, workers, http_handler, ws_handler, sse_handler);
        look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
            "Listening on port " + std::to_string(port));
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[look-fcgi --mode http] " << e.what() << "\n";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", e.what());
    }

#ifndef _WIN32
    if (imap_srv) { imap_srv->stop(); }
    if (smtp_srv) { smtp_srv->stop(); }
    if (smtp_thread.joinable()) smtp_thread.join();
#endif
}

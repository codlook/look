// look-fcgi --mode http
//
// HTTP/1.1 modunda doğrudan TCP dinler — Apache/FastCGI bypass.
// Nginx opsiyonel reverse proxy olarak önünde durabilir.
//
// Mevcut --mode fcgi (default) hiç değişmez.
// Aynı .lk kodu her iki modda çalışır.

#include "look/read_stream.h"   // read_stream — dosyayı tek seferde oku
#include "look/builtins.h"
#include "look/route_group.h"
#include "look/metrics.h"
#include "look/array_count.h"
#include "look/http_server.h"
#include <algorithm>
#include "look/db_async_pool.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/jobs_store.h"
#include "look/runtime_host.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/sse.h"
#include "look/websocket.h"
#include "look/smtp_server.h"
#include "look/imap_server.h"
#include "look/fiber.h"
#include <set>
#include "look/timer.h"
#include "look/vm_closure_runner.h"   // vm_closure_runner (jobs::run VM işleyicilerini çalıştırır)
#include "look/health_path.h"     // yerleşik sağlık yolu — istek + başlangıç çakışma denetimi
#include "look/route_params.h"    // route_check_handler — rota parametre adları kayıtta denetlenir
#include "look/struct_types.h"    // struct_check_type_known — kurulum sonrası alan tipi denetimi

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
#include <unordered_set>
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

// İstemci IP'sini çöz — GERÇEK TCP peer'dan başla, proxy başlıklarına yalnız
// bağlanan IP LOOK_TRUSTED_PROXY listesindeyse güven.
//
// ESKİ HATA: bu mantık rate limiter yolunda DOĞRU yazılmıştı ama request::ip()'yi
// besleyen yerlerde (ctx.remote_addr) KOPYALANMAMIŞ, yerine tek satır konmuştu:
//     if (req.headers.count("x-forwarded-for")) ctx.remote_addr = <baslik>;
// Sonucu (--mode http, ölçüldü):
//   başlık yok                  -> request::ip() = ""        (gerçek peer yok sayılıyor)
//   X-Forwarded-For: 9.9.9.9    -> request::ip() = "9.9.9.9" (İSTEMCİ KENDİ IP'SİNİ SEÇİYOR)
//   X-Forwarded-For: a, b       -> zincir ayrıştırılmıyor
//   X-Forwarded-For: <çöp>      -> doğrulanmadan log'a/sorguya/ban listesine
// IP'ye dayalı yetkilendirme, ban ve denetim kaydı güvenilmez hale geliyordu.
// Artık tek kaynak: üç çağrı yeri de bu fonksiyonu kullanıyor (S2 — kopya sapar).
template <typename Req>
static std::string resolve_client_ip(const Req& req) {
    std::string ip = req.remote_addr;              // gerçek TCP peer
    if (!is_trusted_proxy(ip)) return ip;          // güvenilmeyen kaynak: başlıklara BAKMA
    auto ireal = req.headers.find("x-real-ip");
    if (ireal != req.headers.end()) return ireal->second;
    auto ixff = req.headers.find("x-forwarded-for");
    if (ixff == req.headers.end()) return ip;
    std::string xff = ixff->second;
    auto comma = xff.find(',');                    // "client, proxy1, proxy2" → ilki
    if (comma != std::string::npos) xff = xff.substr(0, comma);
    xff.erase(0, xff.find_first_not_of(" \t"));
    auto last = xff.find_last_not_of(" \t");
    xff.erase(last == std::string::npos ? 0 : last + 1);
    return xff.empty() ? ip : xff;
}

// Is the ORIGINAL client connection HTTPS? The http server itself is plain HTTP (TLS is a front
// proxy's job), so the only signal is X-Forwarded-Proto — and only when it comes from a TRUSTED
// proxy (LOOK_TRUSTED_PROXY). Believing it from anyone would let a client forge the header and
// flip the session cookie's Secure flag. Same trust rule as X-Forwarded-For above.
template <typename Req>
static bool resolve_is_https(const Req& req) {
    if (!is_trusted_proxy(req.remote_addr)) return false;
    auto it = req.headers.find("x-forwarded-proto");
    if (it == req.headers.end()) return false;
    std::string p = it->second;
    auto comma = p.find(','); if (comma != std::string::npos) p = p.substr(0, comma);
    p.erase(0, p.find_first_not_of(" \t"));
    auto last = p.find_last_not_of(" \t"); p.erase(last == std::string::npos ? 0 : last + 1);
    for (auto& c : p) c = (char)std::tolower((unsigned char)c);
    return p == "https";
}

// ── Shared hot-reload state (mirrors WarmApp in fcgi_main) ───────────────────

struct HttpApp {
    // Setup/hot-reload sayacı — her yeniden kurulumda artar. Worker'lardaki
    // thread_local dispatch kopyası bunu görüp kendini tazeler (bkz. per-request
    // yolundaki tl_copy). Sayaç olmadan hot-reload sonrası eski kopya kullanılırdı.
    std::atomic<uint64_t>                    generation{0};

    // ── Modül ev sahibi (RuntimeHost) + program ───────────────────────────────
    std::unique_ptr<look::RuntimeHost>       interp;
    std::unique_ptr<look::Program>           program;
    std::ostringstream                       setup_out;

    // ── Bytecode VM (tek motor — derlenemezse uygulama başlamaz) ──────────────
    std::unique_ptr<look::CompiledProgram>   compiled;

    // VM route registry: setup fazında VM çalıştırılarak doldurulur.
    // pattern = "GET:/path/{id}", closure = compiled closure ptr (BYTECODE_FN value)
    struct VmRouteRaw { std::string pattern; look::Value fn; std::vector<look::Value> middlewares; };
    std::vector<VmRouteRaw>                          vm_routes;
    // VM'nin dispatch için hazır olduğunu gösterir.
    bool                                     vm_routes_ready = false;
    // Rota kaydetmeyen betik (PHP gibi): her istek programın tamamını baştan çalıştırır (VM'de).
    bool                                     script_mode = false;
    // before_route() middleware listesi (setup fazında kaydedilir, dispatch'ten önce çalışır)
    std::vector<look::Value>                 vm_before_routes;
    // Setup VM globals: db_check/json_ok/json_hata/admin_kontrol closure'ları + $conn pool key
    std::unordered_map<std::string, look::Value> vm_setup_globals;

    fs::path                                 script_path;
    fs::file_time_type                       mtime;
};

static HttpApp                  g_http_app;
static std::shared_mutex        g_http_mutex;
static std::atomic<bool>        g_http_ready{false};

// LOOK 2: bu iş parçacığı bir zamanlayıcı geri çağırması koşturuyorsa çağrılacak closure.
// look_app_dispatch bunu görünce rota eşleştirmez, closure'ı istek VM'inde doğrudan çağırır.
// Fiber içinde FİBERE özeldir (FiberLocal::http_direct): işleyici bir sorguda askıya alınınca
// aynı iş parçacığındaki başka bir fiber bunun üstüne yazmasın. Fiber dışında iş parçacığına özel.
struct DirectState {
    const look::Closure* closure = nullptr;
    // ... ve ona verilecek argümanlar (WebSocket mesajı gibi); yoksa argümansız çağrılır.
    const std::vector<look::Value>* args = nullptr;
    // Bir WebSocket/SSE BAĞLANTISININ rota işleyicisi koşuyorsa o bağlantı (WEBSOCKET / SSE_CONN
    // değeri). İstek VM'i bunu görünce işleyiciye ilk argüman olarak verir.
    look::Value conn;
    // Doğrudan çağrılan geri çağırmanın log etiketi ("message handler" …): hata bu adla loglanır.
    const char* what = nullptr;
    // Doğrudan çağrının sonucu ve hata durumu (iş işleyicisi dönüş değerine bakar).
    look::Value result;
    bool failed = false;
};
static thread_local DirectState t_direct_thread;
static DirectState& direct() {
    if (look::Fiber* f = look::Fiber::current()) {
        if (!f->local.http_direct) f->local.http_direct = std::make_shared<DirectState>();
        return *static_cast<DirectState*>(f->local.http_direct.get());
    }
    return t_direct_thread;
}
// Bu iş parçacığında şu an bir dağıtım sürüyor mu (iç içe "yeni istek" ayrı iş parçacığında koşar).
static thread_local int t_dispatch_depth = 0;
static look::Value run_closure_as_request(const look::Value& fnv, std::vector<look::Value> args, const char* what, bool& ok);

// 1.0.8 — Bir rota VM'in SAĞLAMADIĞI bir yerleşik fonksiyona ulaşıyor mu? (rota içinde
// timer::after, jobs::worker ... — derleyici bunları `LOAD_GLOBAL "mod::fn"` + genel CALL olarak
// derler, VM'de değer null olur ve çağrı "Not callable" ile düşer.) Eskiden bu rotalar yalnızca
// "hata olunca aynı isteği yorumlayıcıda yeniden çalıştır" ağı sayesinde çalışıyordu; o ağ
// işleyiciyi İKİ KEZ çalıştırdığı için kalktı. Karar artık ÇALIŞMADAN ÖNCE, kurulumda verilir:
// rotanın proto'su, iç proto'ları ve adıyla çağırdığı global fonksiyonlar (geçişli) taranır;
// VM global'lerinde karşılığı olmayan bir "mod::fn" adı yükleniyorsa uygulama başlamaz (LOOK 2;
// 1.0.8'de rota baştan yorumlayıcıya sabitleniyordu). Değişken üzerinden yapılan dinamik çağrı
// görülemez: o durumda istek 500 alır.
// Saf (isteğe bağlı olmayan) yalın yerleşikler: bool, string, strlen, abs, max, min, sqrt, strtoupper,
// strtolower, join.
// İSTEK tablosu ve KURULUM tablosu aynı tanımı kullanır. Eskiden yalnız istek tablosunda vardılar:
// VM kurulum geçişinde boş değer dönüyor, üst düzeyde `$x = strtoupper("a")` ile hesaplanan
// değişken rotalarda null oluyordu (yorumlayıcıda doğruydu) — sessiz yanlış değer.
static void bind_scalar_builtins(std::vector<look::BuiltinFn>& t) {
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    t[BI("bool")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.empty()) return look::Value(false);
        auto& v = args[0];
        if (v.type() == look::Value::BOOL) return v;
        if (v.type() == look::Value::INT) return look::Value(v.as_int() != 0);
        if (v.type() == look::Value::FLOAT) return look::Value(v.as_float() != 0.0);
        if (v.type() == look::Value::STRING) return look::Value(!v.as_string().empty());
        if (v.type() == look::Value::NONE) return look::Value(false);
        return look::Value(true);
    };
    t[BI("string")] = [](std::vector<look::Value>& args) -> look::Value {   // str ile aynı
        return look::Value(args.empty() ? "" : args[0].to_string());
    };
    t[BI("strlen")] = [](std::vector<look::Value>& args) -> look::Value {
        return look::Value(args.empty() ? 0 : (int)args[0].to_string().size());
    };
    t[BI("abs")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.empty()) return look::Value(0);
        auto& v = args[0];
        if (v.type() == look::Value::FLOAT) return look::Value(std::abs(v.as_float()));
        return look::Value(std::abs(v.to_int()));
    };
    t[BI("max")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.size() < 2) return args.empty() ? look::Value() : args[0];
        return args[0] >= args[1] ? args[0] : args[1];
    };
    t[BI("min")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.size() < 2) return args.empty() ? look::Value() : args[0];
        return args[0] <= args[1] ? args[0] : args[1];
    };
    t[BI("sqrt")] = [](std::vector<look::Value>& args) -> look::Value {
        return look::Value(args.empty() ? 0.0 : std::sqrt(args[0].to_float()));
    };
    t[BI("strtoupper")] = [](std::vector<look::Value>& args) -> look::Value {
        std::string s = args.empty() ? "" : args[0].to_string();
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return look::Value(s);
    };
    t[BI("strtolower")] = [](std::vector<look::Value>& args) -> look::Value {
        std::string s = args.empty() ? "" : args[0].to_string();
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return look::Value(s);
    };
    // bare join(arr, sep="") — global builtin (use gerektirmez).
    t[BI("join")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.empty() || args[0].type() != look::Value::ARRAY)
            return look::Value(args.empty() ? std::string() : args[0].to_string());
        std::string sep = args.size() >= 2 ? args[1].to_string() : "";
        std::string result;
        auto& arr = *args[0].as_array();
        for (size_t i = 0; i < arr.size(); ++i) { if (i) result += sep; result += arr[i].to_string(); }
        return look::Value(result);
    };
}

static bool proto_needs_interpreter(const look::FunctionProto* p,
                                    const std::unordered_map<std::string, look::Value>& globals,
                                    std::unordered_set<const look::FunctionProto*>& seen,
                                    std::string& which) {
    if (!p || !seen.insert(p).second) return false;
    for (const auto& ins : p->code) {
        if (ins.op != look::OpCode::LOAD_GLOBAL) continue;
        const size_t ni = ins.bx();
        if (ni >= p->constants.size() || p->constants[ni].type() != look::Value::STRING) continue;
        const std::string& name = p->constants[ni].str_ref();
        auto g = globals.find(name);
        if (g == globals.end()) {
            if (name.find("::") != std::string::npos) { which = name; return true; }
            continue;
        }
        if (g->second.type() == look::Value::BYTECODE_FN)
            if (auto cl = g->second.as_bytecode_fn())
                if (proto_needs_interpreter(cl->proto.get(), globals, seen, which)) return true;
    }
    for (const auto& n : p->nested)
        if (proto_needs_interpreter(n.get(), globals, seen, which)) return true;
    return false;
}

// Açılış bilgi satırları ([BYTECODE] …, "VM dispatch ready") — uzun ömürlü sunucu için anlamlı.
// lk-cgi her istekte yeni süreçtir: stderr Apache'nin hata günlüğüne gider → orada her istekte
// gürültü olurdu (eski lk-cgi başarılı istekte stderr'e hiçbir şey yazmıyordu). CGI kapatır;
// uyarı ve hatalar her zaman yazılır.
static bool g_http_quiet_startup = false;
void look_http_engine_set_quiet(bool quiet) { g_http_quiet_startup = quiet; }
static void startup_note(const std::string& stderr_line, const std::string& log_line = "") {
    if (g_http_quiet_startup) return;
    std::cerr << stderr_line << "\n";
    if (!log_line.empty()) look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", log_line);
}

// LOOK 2 tek motor: program derlenemiyorsa uygulama BAŞLAMAZ (eskiden bütün uygulama sessizce
// ~40× yavaş yorumlayıcıya düşüyordu; LOOK_VM_STRICT yalnız bunu reddetmenin yoluydu — artık
// tek davranış budur). Derleme hatası programın hatasıdır (eksik dosya, bilinmeyen modül, sınır
// aşımı) — mesaj olduğu gibi iletilir; "VM çalıştıramıyor" çerçevesi motoru suçlu gösteriyordu.
// İstisna fırlatır: başlangıçta çağıran mesajı yazıp 1 koduyla çıkar; sıcak yeniden yüklemede
// hata loglanır, sunucu kapanmaz (uygulama hazır değil → istekler 500 alır, dosya düzeltilince
// sonraki yükleme toparlar).
[[noreturn]] static void refuse_start(const std::string& why) {
    g_http_app.vm_routes_ready = false;
    throw std::runtime_error(why);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("Could not open file: " + p.string());
    return look::read_stream(f);
}

static void run_setup_http(const fs::path& script) {
    look::clear_db_pools();

    auto src = read_file(script);

    look::Lexer  lexer(src);
    auto tokens = lexer.scan_tokens();

    look::Parser parser(std::move(tokens));
    auto program = parser.parse();
    look::log_param_writes(parser.warnings(), script.string());

    g_http_app.setup_out.str("");
    g_http_app.setup_out.clear();

    static look::WebContext setup_ctx;
    setup_ctx        = look::WebContext{};
    setup_ctx.method = "__SETUP__";

    // LOOK 2 tek motor: üst düzey kod (kurulum) YALNIZ VM'de, bir kez çalışır. Bu nesne LOOK kodu
    // ÇALIŞTIRMAZ — yalnız yerleşik modüllerin deposu (ModuleHost): modül fonksiyonları ondan
    // alınır, web modülleri kurulum bağlamına (setup_ctx) bağlanır, setup modu bayrağını taşır.
    // (Eskiden üst düzey önce burada yorumlayıcıda koşuyor, sonra VM aynı kodu kayıttan geri
    // oynatıyordu: iki geçiş, iki motor.)
    auto interp = std::make_unique<look::RuntimeHost>(g_http_app.setup_out);
    interp->set_file(script.string());
    interp->set_web_context(&setup_ctx);
    // Modüller derlemeden SONRA açılır (aşağıda, CompiledProgram::use_modules).
    g_http_app.program     = std::move(program);
    g_http_app.interp      = std::move(interp);
    g_http_app.script_path = script;

    // ── Bytecode compile — LOOK 2: tek motor, yedek yok ───────────────────────
    // LOOK_BYTECODE=0 eskiden bütün uygulamayı yorumlayıcıda çalıştırırdı; artık anlamı yok.
    {
        static bool warned = false;
        const char* e = std::getenv("LOOK_BYTECODE");
        if (e && std::string(e) == "0" && !warned) {
            warned = true;
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "LOOK_BYTECODE=0 is ignored: LOOK 2 runs only on the VM.");
        }
    }

    {
        try {
            startup_note("[BYTECODE] Compiling...");
            auto compiled = std::make_unique<look::CompiledProgram>(
                look::Compiler::compile(*g_http_app.program,
                                        g_http_app.script_path.parent_path().string()));
            // Yalnız programın `use` ettiği stdlib modülleri açılır (dahil edilen dosyalarınkiler de —
            // derleyici zaten okuduğu `use` satırlarından toplar; CLI ile aynı liste). `use` edilmeyen
            // modülün yuvası boş kalır → VM "Module 'x' not loaded." verir. Paket modülü
            // (~/.look/modules) stdlib değildir: derleyici derler.
            for (const auto& m : compiled->use_modules) g_http_app.interp->load_stdlib_module(m);
            g_http_app.compiled     = std::move(compiled);
            startup_note("[BYTECODE] OK — VM mode active", "Bytecode compile OK — VM mode active");
        } catch (const std::exception& e) {
            refuse_start(e.what());
        } catch (...) {
            refuse_start("the program could not be compiled");
        }

    // ── VM Setup Fazı: bytecode'u çalıştır, route'ları kaydet ────────────────
    g_http_app.vm_routes.clear();
    g_http_app.vm_before_routes.clear();
    g_http_app.vm_routes_ready = false;
    g_http_app.script_mode     = false;
    if (g_http_app.compiled) {
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
            setup_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {  // count/len
                return look::Value(args.empty() ? 0 : look::look_count(args[0]));  // tek tanım (array_count.h)
            };

            // push (3) — setup'ta $routes_list push'ları için
            setup_builtins[3] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                args[0].detach();   // LOOK 2: paylaşılan depoyu yerinde değiştirme (değişken yolu PUSH_PATH ile gelir)
                args[0].as_array()->push_back(args[1]);
                return look::Value();
            };
            // pop (4)
            setup_builtins[4] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty() || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                args[0].detach();
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
                // to_int() KULLAN — stoll(to_string(float)) BOZUK: to_string büyük float'ı
                // bilimsel gösterime çevirir ("1e+06"), stoll 'e'de durup 1 döndürür →
                // int(float>~1e6) sessizce yanlış. to_int() FLOAT'ı doğrudan (int64_t) cast
                // eder, STRING'i aynı stoll ile parse eder.
                return look::Value(args[0].to_int());
            };
            // float (7)
            setup_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value(0.0);
                // to_float(): int()'teki gibi to_string() round-trip'i ele (FLOAT doğrudan,
                // string round-trip hassasiyet kaybı yok).
                return look::Value(args[0].to_float());
            };

            bind_scalar_builtins(setup_builtins);   // strlen, abs, max, min, sqrt, strtoupper, strtolower, join — istek tablosuyla aynı tanım

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

            // route::group(prefix, [mw], fn) — grup context'ini it, closure'ı çağır (invoke aktif
            // setup VM'e köprüler), çıkar. İçindeki route() çağrıları prefix+mw miras alır.
            setup_builtins[(size_t)look::builtin_index("route::group")] =
                [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() < 2) return look::Value();
                std::string prefix = args[0].to_string();
                std::vector<look::Value> gmws;
                look::Value cb;
                if (args.size() >= 3) {
                    if (args[1].type() == look::Value::ARRAY) gmws = *args[1].as_array();
                    cb = args[2];
                } else {
                    cb = args[1];
                }
                look::route_group_push(prefix, std::move(gmws));
                try { g_http_app.interp->invoke(cb, {}); }
                catch (...) { look::route_group_pop(); throw; }
                look::route_group_pop();
                return look::Value();
            };

            // route() — index 22: pattern + closure kaydet
            setup_builtins[22] = [vm_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                // route("GET", "/path", fn) — 3-arg form
                // route("GET", "/path", [mw...], fn) — 4-arg form
                // route("404", fn) — 2-arg form
                if (args.size() >= 4) {
                    // 4-arg: method, pattern, middlewares, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = look::route_group_prefix() + args[1].to_string();
                    // grup middleware'leri (dış→iç) ÖNE, sonra route'un kendi mw'leri
                    std::vector<look::Value> middlewares = look::route_group_middlewares();
                    if (args[2].type() == look::Value::ARRAY) {
                        auto& own = *args[2].as_array();
                        middlewares.insert(middlewares.end(), own.begin(), own.end());
                    }
                    if (args[3].type() == look::Value::BYTECODE_FN) {
                        look::route_check_handler(method, pattern, args[3].as_bytecode_fn()->proto->params);
                        look::route_check_builtin_path(method, pattern);
                        vm_routes_ptr->push_back({method + ":" + pattern, args[3], std::move(middlewares)});
                    }
                } else if (args.size() >= 3) {
                    // 3-arg: method, pattern, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = look::route_group_prefix() + args[1].to_string();
                    std::vector<look::Value> middlewares = look::route_group_middlewares();
                    if (args[2].type() == look::Value::BYTECODE_FN) {
                        look::route_check_handler(method, pattern, args[2].as_bytecode_fn()->proto->params);
                        look::route_check_builtin_path(method, pattern);
                        vm_routes_ptr->push_back({method + ":" + pattern, args[2], std::move(middlewares)});
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

            // KRİTİK: override'lar İSİMDEN indekslenir (builtin_index). Eskiden hard-coded sayılardı;
            // builtin_names()'e giriş eklenince indeksler kayıyordu (sessiz yanlış fonksiyon).
            // db::connect ve diğer modül fonksiyonları aşağıdaki genel döngüde GERÇEK modül
            // fonksiyonuna bağlanır (kurulum tek geçiş: havuz bir kez açılır).
            auto SBI = [](const char* n) { return (size_t)look::builtin_index(n); };
            setup_builtins[SBI("channel")] = [](std::vector<look::Value>& args) -> look::Value {
                int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
                if (buf <= 0) buf = (1 << 20);
                return look::Value(std::make_shared<look::LookChannel>(buf));
            };
            setup_builtins[SBI("args")] = [](std::vector<look::Value>&) -> look::Value {
                return look::Value(std::make_shared<std::vector<look::Value>>());
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

            // ── Kalan yerleşikler: GERÇEK modül fonksiyonları (LOOK 2: kurulum TEK geçiş) ─────────
            // Üst düzey yalnız burada, VM'de, bir kez çalışır: modül fonksiyonlarının yan etkileri
            // (db::exec, file::put, cache::set, http::get …) bir kez gerçekleşir, dönüşleri gerçektir.
            // Eskiden yorumlayıcı geçişi bunları çalıştırıyor, VM geçişi sonuçları kayıttan geri
            // oynatıyordu (1.0.9) — iki geçiş, iki motor; artık ikisi de yok.
            {
                const auto& bnames = look::builtin_names();
                look::RuntimeHost* host = g_http_app.interp.get();
                for (size_t i = 0; i < setup_builtins.size() && i < bnames.size(); ++i) {
                    if (setup_builtins[i]) continue;           // yukarıda özel olarak bağlandı
                    const std::string& name = bnames[i];
                    // Üst düzeyde kurulan zamanlayıcı: geri çağırma VM closure'ı, her tetiklenişte
                    // yeni bir istek gibi kurulan VM'de çalışır (rotada kurulanla aynı yol).
                    if (name == "timer::after" || name == "timer::every") {
                        const bool repeat = (name == "timer::every");
                        setup_builtins[i] = [name, repeat](std::vector<look::Value>& args) -> look::Value {
                            if (args.size() < 2) throw std::runtime_error(name + "() takes 2 arguments");
                            if (args[1].type() != look::Value::BYTECODE_FN)
                                throw std::runtime_error(name + "() second argument must be a function");
                            const int ms = (int)args[0].to_int();
                            look::Value fnv = args[1].clone_for_thread();
                            auto cb = [fnv]() { bool ok = false; run_closure_as_request(fnv, {}, "timer callback", ok); };
                            return look::Value(repeat ? look::TimerManager::instance().every(ms, std::move(cb))
                                                      : look::TimerManager::instance().after(ms, std::move(cb)));
                        };
                        continue;
                    }
                    if (name == "timer::wait") {
                        setup_builtins[i] = [](std::vector<look::Value>&) -> look::Value {
                            throw std::runtime_error("timer::wait() can be used only on the command line: a web application does not wait for timers");
                        };
                        continue;
                    }
                    if (name == "timer::cancel") {
                        setup_builtins[i] = [](std::vector<look::Value>& args) -> look::Value {
                            if (args.empty()) throw std::runtime_error("timer::cancel() takes 1 argument");
                            look::TimerManager::instance().cancel((int)args[0].to_int());
                            return look::Value();
                        };
                        continue;
                    }
                    // jobs::run — işleyicileri çalıştırır (VM işleyicileri vm_closure_runner ile).
                    if (name == "jobs::run") {
                        setup_builtins[i] = [host](std::vector<look::Value>& args) -> look::Value {
                            return host->jobs_run(args.empty() ? 5000 : (int)args[0].to_float());
                        };
                        continue;
                    }
                    auto colon = name.find("::");
                    // `use` edilmemiş modül: yuva boş kalır, VM "Module 'x' not loaded." der.
                    if (colon != std::string::npos && host->module_not_used(name.substr(0, colon))) continue;
                    if (colon != std::string::npos) {
                        auto f = host->get_module_fn(name.substr(0, colon), name.substr(colon + 2));
                        if (f) {
                            // jobs::worker: işleyici başka iş parçacıklarında çağrılır → closure'u kopyala.
                            const bool clone_fn_arg = (name == "jobs::worker");
                            setup_builtins[i] = [f, clone_fn_arg](std::vector<look::Value>& args) -> look::Value {
                                std::vector<look::Value> a = args;
                                if (clone_fn_arg && a.size() >= 2 && a[1].type() == look::Value::BYTECODE_FN)
                                    a[1] = a[1].clone_for_thread();
                                return f(a);
                            };
                            continue;
                        }
                    }
                    // Kurulumda anlamı olmayan yerleşik (ws::on, istek sırasında var olan bağlam …):
                    // açık hata — eskiden sessizce null dönüyordu.
                    setup_builtins[i] = [name](std::vector<look::Value>&) -> look::Value {
                        throw look::LookVmError(name + "() cannot be used at the top level of a web application");
                    };
                }
            }
            // jobs::run kurulumda çağrılırsa VM işleyicilerini çalıştırabilsin (kurulumdan ÖNCE kur).
            look::vm_closure_runner() = run_closure_as_request;

            look::VM::SharedState setup_sh;
            setup_sh.builtins = &setup_builtins;
            std::ostringstream vm_setup_out;
            look::VM setup_vm(setup_sh, vm_setup_out);
            // Kurulum sürüyor: db::query gibi modüller bunu ModuleHost::is_setup_mode ile görür.
            struct SetupMode { look::RuntimeHost* h; SetupMode(look::RuntimeHost* x) : h(x) { h->set_setup_mode(true); }
                               ~SetupMode() { h->set_setup_mode(false); } } setup_mode{g_http_app.interp.get()};
            setup_vm.execute(*g_http_app.compiled);

            // Setup globals'ı sakla: db_check/json_ok/json_hata gibi named fn closure'lar +
            // $conn pool key — dispatch VM'e set_globals() ile verilecek
            g_http_app.vm_setup_globals = setup_vm.get_globals();

            // LOOK 2: tüm dosyalar yüklendi, bütün struct'lar biliniyor → alan tip adlarını ŞİMDİ
            // denetle. Yazım hatası (`strng`) uygulama BAŞLARKEN hata verir; yoksa ancak o struct'ı
            // kuran (belki nadiren çağrılan) rota çalıştığında, üretimde ortaya çıkardı. Tanımlar
            // VM global'lerinde "__sdef:<ad>" altında [alan, varsayılan, tip] üçlüleri olarak durur.
            {
                const std::string pre = "__sdef:";
                auto known = [&](const std::string& t) {
                    return g_http_app.vm_setup_globals.count(pre + t) != 0;
                };
                for (const auto& [k, v] : g_http_app.vm_setup_globals) {
                    if (k.rfind(pre, 0) != 0 || v.type() != look::Value::ARRAY) continue;
                    const auto& f = *v.as_array();
                    for (size_t j = 0; j + 2 < f.size(); j += 3)
                        look::struct_check_type_known(k.substr(pre.size()), f[j].to_string(), f[j + 2].to_string(), known);
                }
            }

            // runtime::stats()["routes"]. ESKİ HATA (4b'den beri): sayı yorumlayıcının rota kaydından
            // okunuyordu; kurulum VM'e geçince o kayıt boş kaldı ve stats web'de 0 rota diyordu.
            g_http_app.interp->set_route_count((int)g_http_app.vm_routes.size());
            if (!g_http_app.vm_routes.empty()) {
                g_http_app.vm_routes_ready = true;
                // LOOK 2 (tek motor): bir rota ya da before_route VAR OLMAYAN bir modül fonksiyonuna
                // ulaşıyorsa uygulama BAŞLAMAZ. Her modül fonksiyonu VM'de vardır (`lk --vm-report`, CI
                // kapısı); VM'in tanımadığı bir `mod::fn` adı yazım hatasıdır. Eskiden böyle bir rota
                // sessizce yorumlayıcıya alınıyor, hata ancak o rotaya ilk istek geldiğinde çıkıyordu.
                {
                    auto needs = [&](const look::Value& fn, std::string& which) {
                        if (fn.type() != look::Value::BYTECODE_FN) return false;
                        auto cl = fn.as_bytecode_fn();
                        std::unordered_set<const look::FunctionProto*> seen;
                        return cl && proto_needs_interpreter(cl->proto.get(), g_http_app.vm_setup_globals, seen, which);
                    };
                    std::string mw_which;
                    bool mw_needs = false;
                    for (const auto& mw : g_http_app.vm_before_routes)
                        if (needs(mw, mw_which)) { mw_needs = true; break; }
                    for (size_t i = 0; i < g_http_app.vm_routes.size(); ++i) {
                        std::string which = mw_which;
                        bool n = mw_needs || needs(g_http_app.vm_routes[i].fn, which);
                        for (const auto& mw : g_http_app.vm_routes[i].middlewares)
                            if (!n && needs(mw, which)) n = true;
                        if (!n) continue;
                        // İstisna — std::exit DEĞİL: ilk açılışta çağıran yazıp 1 koduyla çıkar; sıcak
                        // yeniden yüklemede sunucu ayakta kalır (istekler 500); lk-cgi bir hata yanıtı
                        // yazar. ESKİ HATA: exit(1) yeniden yüklemede bütün sunucuyu öldürüyordu.
                        g_http_app.vm_routes_ready = false;
                        throw std::runtime_error("route " + g_http_app.vm_routes[i].pattern + " calls " + which +
                            "(), which does not exist — check the spelling of the module and the function.");
                    }
                }
                startup_note("[BYTECODE] VM routes: " + std::to_string(g_http_app.vm_routes.size()) + " registered",
                             "VM dispatch ready — " + std::to_string(g_http_app.vm_routes.size()) + " route");
            } else {
                // Rota kaydetmeyen betik (PHP gibi): her istek programın tamamını VM'de baştan çalıştırır.
                g_http_app.vm_routes_ready = true;
                g_http_app.script_mode     = true;
                startup_note("[BYTECODE] no routes — each request runs the whole script on the VM");
            }
        } catch (const std::exception& e) {
            // Kurulum yalnız VM'de koşuyor: buradaki hata programın KENDİ hatasıdır (eksik argüman,
            // throw, `use` edilmemiş modül …), "VM bu programı çalıştıramıyor" değil — mesaj olduğu
            // gibi iletilir (çağıran "Setup error: …" diye yazar, uygulama başlamaz).
            g_http_app.vm_routes_ready = false;
            throw std::runtime_error(e.what());
        }
    }
    }
    g_http_app.mtime       = fs::last_write_time(script);

    // Kurulum/hot-reload tamamlandı → worker'lardaki thread_local dispatch kopyaları
    // bu artışı görüp kendilerini yeniden kuracak (eski interp'e asılı kalmasınlar).
    g_http_app.generation.fetch_add(1, std::memory_order_release);

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

// Dağıtım modeli (1.0.20): Linux'ta VARSAYILAN fiber — bağlantı beklerken işçiyi tutmaz.
// Eski model (bağlantı başına bir işçi) yalnız LOOK_FIBER_DISPATCH=0 ile seçilir. O modelde
// işçi sayısı kadar açık bağlantı (boşta bekleyen tarayıcı bağlantıları dahil) sunucuyu
// zaman aşımı dolana dek kilitliyordu. Linux dışında fiber dağıtımı yok: yalnız "1" açar.
static bool look_fiber_dispatch_on(const char* v) {
#ifdef __linux__
    return !(v && std::string(v) == "0");
#else
    return v && std::string(v) == "1";
#endif
}

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

    // Remote addr — request::ip() bunu okur. Gerçek peer'dan başlar, proxy
    // başlıklarına yalnız güvenilir proxy arkasındaysa güvenir (bkz.
    // resolve_client_ip: eskiden burada başlık KOŞULSUZ kabul ediliyordu).
    ctx.remote_addr = resolve_client_ip(req);
    ctx.is_https    = resolve_is_https(req);

    // Gövde ayrıştırma — urlencoded VE multipart, tek kaynaktan (web.cpp).
    // ESKİ HATA: burada yalnızca urlencoded dalı vardı, multipart hiç yoktu →
    // `request::file()` FastCGI'de çalışıyor, `--mode http`'de "requires
    // multipart/form-data request" hatası veriyordu. Aynı uygulama moda göre
    // farklı davranıyordu (S12 — moda özgü boşluk); README'de "bilinen sınır"
    // diye yazılmıştı, oysa unutulmuş bir daldı.
    // Method kapısı da kaldırıldı: FastCGI tarafı hiç method kontrol etmiyordu,
    // bu yüzden PUT/PATCH gövdeleri iki modda farklı davranıyordu.
    if (!req.body.empty()) ctx.parse_post_body();

    return ctx;
}

// ── Request handler called by HttpServer worker threads ───────────────────────

// İleri bildirim — tanım http_handler'dan sonra (paylaşılan motor)
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path);

// LOOK 2 — bir VM closure'ını YENİ BİR İSTEK gibi çalıştırır: kendi VM'i, kendi builtin'leri,
// global'lerin bildirilen değerleri (üst düzeyde kaydedilen zamanlayıcı ve iş işleyicileri için).
// Hata loglanır (what + " error: …"), ok=false döner; hiçbir şey yeniden çalıştırılmaz.
static look::Value run_closure_here(const look::Value& fnv, std::vector<look::Value>& args,
                                    const char* what, bool& ok) {
    look::WebContext ctx;
    ctx.method = "__EVENT__";
    std::ostringstream sink;
    const look::Closure* pc = direct().closure;
    const std::vector<look::Value>* pa = direct().args;
    const char* pw = direct().what;
    direct().closure = fnv.as_bytecode_fn().get();
    direct().args    = &args;
    direct().what    = what;
    direct().failed  = false;
    direct().result  = look::Value();
    try { look_app_dispatch(ctx, sink, std::string()); }
    catch (const std::exception& ex) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", std::string(what) + " error: " + ex.what());
        direct().failed = true;
    } catch (...) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", std::string(what) + " unknown error");
        direct().failed = true;
    }
    ok = !direct().failed;
    look::Value r = std::move(direct().result);
    direct().result  = look::Value();
    direct().closure = pc;
    direct().args    = pa;
    direct().what    = pw;
    return r;
}
static look::Value run_closure_as_request(const look::Value& fnv, std::vector<look::Value> args,
                                          const char* what, bool& ok) {
    // Bu iş parçacığında zaten bir istek dağıtılıyorsa (ör. bir rota jobs::run çağırdı) iç içe
    // dağıtım look_app_dispatch'te ele alınır: dış isteğin önbellekleri ve veritabanı
    // bağlantıları korunur. (Ayrı iş parçacığı açmak, her tüketicinin bir bağlantı tuttuğu
    // durumda havuzu tüketip kilitleniyordu — tests/jobs_concurrent_test.sh.)
    return run_closure_here(fnv, args, what, ok);
}

// Hot-reload throttle: the per-request mtime stat (fs::last_write_time) + shared_lock
// was the dominant per-request cost on mounted/slow filesystems — strace showed one
// stat() on the script for EVERY request. A dev convenience does not belong in the hot
// path at that price. Throttle it to at most once per interval (default 500ms — a reload
// still feels instant in dev); the vast majority of requests then pay a single lock-free
// atomic read. Escape hatch: LOOK_HOT_RELOAD_MS (0 = check every request, legacy behaviour).
static std::atomic<long long> g_last_hotcheck_ms{0};
static long long hot_reload_interval_ms() {
    static long long ms = []() -> long long {
        const char* e = std::getenv("LOOK_HOT_RELOAD_MS");
        if (e && e[0]) { long long v = std::atoll(e); return v < 0 ? 500 : v; }
        return 500;
    }();
    return ms;
}

static void http_handler(const look::HttpRequest& req, look::HttpResponse& resp) {
    // Readiness — cheap lock-free atomic (g_http_ready flips false only mid hot-reload).
    if (!g_http_ready.load(std::memory_order_acquire)) {
        resp.status_code = 503;
        resp.status_text = "Service Unavailable";
        resp.body = "{\"ok\":false,\"error\":\"Starting\"}";
        return;
    }
    // Throttled hot-reload check (see note above).
    {
        long long interval = hot_reload_interval_ms();
        bool do_check;
        if (interval <= 0) {
            do_check = true;   // legacy: stat every request
        } else {
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            long long last = g_last_hotcheck_ms.load(std::memory_order_relaxed);
            // One thread wins the check slot per interval via CAS; others skip.
            do_check = (now_ms - last >= interval) &&
                       g_last_hotcheck_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed);
        }
        if (do_check) {
            std::shared_lock<std::shared_mutex> sl(g_http_mutex);
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
                        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", std::string("Hot reload error: ") + e.what());
                    }
                }
            }
        }
    }

    // ── Rate limit check ─────────────────────────────────────────────────────
    {
        // Gerçek TCP peer'dan başla; proxy başlıklarına yalnız bağlanan IP
        // LOOK_TRUSTED_PROXY listesindeyse güven. Bu mantık burada DOĞRUYDU ama
        // request::ip() yolunda kopyalanmamıştı ve o kopya sapmıştı — artık her
        // ikisi de tek kaynağı (resolve_client_ip) kullanıyor.
        std::string client_ip = resolve_client_ip(req);
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
        if (req.method == "GET" && req.path == look::health_path()) {
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

// Paylaşılan istek motoru — web ctx hazırlanmış olmalı; VM ile dispatch eder,
// çıktıyı output/web.response_body'ye yazar.
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path) {
    // İÇ İÇE dağıtım: bir istek sürerken aynı iş parçacığında bir VM closure'ı "yeni istek"
    // olarak koşuyor (rota jobs::run çağırdı → iş işleyicisi). Dış isteğin iş parçacığına özgü
    // durumu korunur: builtin tablosu yerel kurulur (önbelleğe dokunulmaz), veritabanı
    // bağlantıları dıştakinden kullanılır (yeniden alınmaz/bırakılmaz — havuzu tüketip
    // kilitlenmesin), çıkışta kopyanın çıktısı ve bağlamı dış isteğe geri bağlanır.
    // Fiber modunda derinlik ve dış bağlam FIBER'a aittir: bir iş parçacığı birçok isteği
    // fiber olarak sırayla askıya alıp sürdürür; thread_local sayaç askıdaki başka isteği
    // "dış istek" sanıyordu (bkz. FiberLocal::dispatch_depth).
    static thread_local look::WebContext*   t_cur_web = nullptr;
    static thread_local std::ostringstream* t_cur_out = nullptr;
    look::Fiber* const cur_fiber = look::Fiber::current();
    int&   depth   = cur_fiber ? cur_fiber->local.dispatch_depth : t_dispatch_depth;
    void*  thr_web = t_cur_web;
    void*  thr_out = t_cur_out;
    void*& cur_web = cur_fiber ? cur_fiber->local.dispatch_web : thr_web;
    void*& cur_out = cur_fiber ? cur_fiber->local.dispatch_out : thr_out;
    struct DepthGuard {
        int& d;
        std::function<void()> restore;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; if (restore) restore(); }
    } depth_guard(depth);
    const bool nested = depth > 1;
    look::WebContext*   const outer_web = static_cast<look::WebContext*>(cur_web);
    std::ostringstream* const outer_out = static_cast<std::ostringstream*>(cur_out);
    cur_web = &web; cur_out = &output;
    if (!cur_fiber) { t_cur_web = &web; t_cur_out = &output; }
    struct CurGuard {
        void*& w; void*& o; void* ow; void* oo; bool thr;
        ~CurGuard() {
            w = ow; o = oo;
            if (thr) { t_cur_web = static_cast<look::WebContext*>(ow); t_cur_out = static_cast<std::ostringstream*>(oo); }
        }
    } cur_guard{cur_web, cur_out, outer_web, outer_out, cur_fiber == nullptr};

    std::shared_lock<std::shared_mutex> sl(g_http_mutex);
    // vm_routes_ready: VM'nin dispatch_routes() kullanabilmesi için
    // native function wiring tamamlanmış olmalı (Phase 16.5 Adım 5.5)
    bool use_bc = g_http_app.compiled && g_http_app.vm_routes_ready;   // başarısız yüklemede vm_routes_ready false

    auto t_copy_start    = std::chrono::steady_clock::now();
    auto t_copy_end      = t_copy_start;
    auto t_dispatch_start = t_copy_start;

    if (use_bc) {
        // ── VM path ───────────────────────────────────────────────────────────
        // ── Dispatch kopyası: worker thread başına BİR KEZ (per-request DEĞİL) ────
        // Ölçüm (docker, 2 worker): make_dispatch_copy() her istekte 30µs (global'siz
        // app) — route'u ÇALIŞTIRMAKTAN (12µs) 2.5× pahalı; global-ağır app'te 63µs ve
        // throughput 21.2k→12.8k düşüyordu (route AYNI olmasına rağmen). Sebep:
        // make_dispatch_copy → make_unique<RuntimeHost>() TÜM stdlib'i (yüzlerce
        // std::function) her istekte yeniden kuruyor + globals_ deep-clone.
        // PHP-FPM istek başına böyle bir bedel ödemez — "PHP'yi geç" hedefinin önündeki
        // asıl engel buydu, motor hızı değil.
        //
        // Güvenlik: her worker tek isteği aynı anda işler → thread_local kopya güvenli.
        // Hot-reload'da generation artar → kopya tazelenir.
        // İZOLASYON: RuntimeHost LOOK global'i tutmaz (VM kendi globals'ını her istekte
        // g_http_app.vm_setup_globals'tan alır) → klon gereksiz.
        t_copy_start = std::chrono::steady_clock::now();
        static thread_local std::unique_ptr<look::RuntimeHost> tl_copy;
        static thread_local uint64_t tl_gen = (uint64_t)-1;
        const uint64_t cur_gen = g_http_app.generation.load(std::memory_order_acquire);
        if (!tl_copy || tl_gen != cur_gen) {
            tl_copy = g_http_app.interp->make_dispatch_copy();
            tl_gen  = cur_gen;
        }
        look::RuntimeHost* copy = tl_copy.get();
        t_copy_end = std::chrono::steady_clock::now();
        copy->set_output(output);
        copy->set_web_context(&web);
        if (nested && outer_web && outer_out)
            depth_guard.restore = [copy, outer_web, outer_out]() { copy->set_output(*outer_out); copy->set_web_context(outer_web); };

        // VM routes'u shared_lock altında alıyoruz (g_http_app read-only erişim).
        // PERF: route_closures listesi vm_routes'tan türetilir (istek-bağımsız, yalnız
        // hot-reload'da değişir) — tl_copy gibi thread_local + generation-cache. Eskiden
        // HER İSTEKTE N VmRoute (pattern string kopyası + middleware vector) + stable_partition
        // kuruluyordu (N-route app'te O(N) per request). Artık yalnız generation değişince
        // yeniden kurulur. Ham pointer'lar (vr.fn) generation-içi geçerli (vm_routes sabit);
        // generation artınca cache tazelenir — tl_copy ile aynı güvenlik.
        static thread_local std::vector<look::VmRoute> tl_route_closures;
        static thread_local uint64_t tl_routes_gen = (uint64_t)-1;
        if (tl_routes_gen != cur_gen) {
            tl_route_closures.clear();
            tl_route_closures.reserve(g_http_app.vm_routes.size());
            for (size_t ri = 0; ri < g_http_app.vm_routes.size(); ++ri) {
                auto& r = g_http_app.vm_routes[ri];
                if (r.fn.type() != look::Value::BYTECODE_FN) continue;
                look::VmRoute vr;
                vr.pattern   = r.pattern;
                vr.fn        = r.fn.as_bytecode_fn().get();
                for (auto& mw : r.middlewares)
                    if (mw.type() == look::Value::BYTECODE_FN)
                        vr.middlewares.push_back(mw.as_bytecode_fn().get());
                tl_route_closures.push_back(std::move(vr));
            }
            // Statik route'ları ({param} içermeyenler) dinamiklerden ÖNCE sırala:
            // /user/new, /user/{id}'den önce eşleşsin (deterministik routing).
            std::stable_partition(tl_route_closures.begin(), tl_route_closures.end(),
                [](const look::VmRoute& r){ return r.pattern.find('{') == std::string::npos; });
            tl_routes_gen = cur_gen;
        }
        std::vector<look::VmRoute>& route_closures = tl_route_closures;
        sl.unlock();

        // Per-request builtins: module fonksiyonları dispatch kopyasından (RuntimeHost) alınır
        // (copy->set_web_context(&web) çağrıldığında modules_ yeni web'e bind edildi)
        //
        // PERF: this table was rebuilt every request — ~106 std::function heap allocations
        // per request (measured: 53% of the fixed per-request cost, the largest single
        // source). Every wrapper captures only generation-stable state (module fns from the
        // thread_local dispatch copy), so — like tl_copy / tl_route_closures beside it — it is
        // cacheable per generation. The one exception is print, which needs the per-request
        // output stream; under the default worker-pool model (one request per thread at a
        // time) it routes through copy->output() (set per request), so the whole table caches
        // thread_local. Under opt-in fiber mode a thread interleaves many fibers, so the
        // thread_local cache and copy->output() would leak one fiber's output into another —
        // there we keep rebuilding per request with print capturing the fiber-local output.
        static const bool g_use_fiber = [](){ const char* v = std::getenv("LOOK_FIBER_DISPATCH");
                                              return look_fiber_dispatch_on(v); }();
        static thread_local std::vector<look::BuiltinFn> tl_req_builtins;
        static thread_local uint64_t tl_builtins_gen = (uint64_t)-1;
        // Modül sarmalayıcıları (request::, response::, session:: …) kuruldukları andaki `web`
        // ADRESİNE bağlıdır. HTTP isteklerinde bu adres iş parçacığı başına sabittir (aynı yığın
        // çerçevesi); ama WS/SSE işleyicisi ve geri çağırmalar BAŞKA çerçeveden dağıtılır. Adres
        // değiştiyse tablo yeniden kurulur — yoksa sonraki istek ölü bir bağlama yazar (ölçüldü:
        // SSE + WS'den sonraki sıradan istekte segfault).
        static thread_local const look::WebContext* tl_builtins_web = nullptr;
        std::vector<look::BuiltinFn> fiber_req_builtins;
        const bool local_table = g_use_fiber || nested;   // iç içe dağıtım dış isteğin önbelleğine dokunmaz
        std::vector<look::BuiltinFn>& req_builtins = local_table ? fiber_req_builtins : tl_req_builtins;
        const bool need_build = local_table || (tl_builtins_gen != cur_gen) || (tl_builtins_web != &web);
        if (need_build) {
        req_builtins.assign(look::builtin_names().size(), look::BuiltinFn{});

        // print/write (0, 1)
        // print/write: fiber mode captures the fiber-local output (safe under interleaving);
        // worker-pool mode routes through copy->output() (set per request) so the table caches.
        if (local_table)
            req_builtins[0] = [&output](std::vector<look::Value>& args) -> look::Value {
                for (auto& a : args) output << a.to_string();
                return look::Value();
            };
        else
            req_builtins[0] = [copy](std::vector<look::Value>& args) -> look::Value {
                for (auto& a : args) copy->output() << a.to_string();
                return look::Value();
            };
        req_builtins[1] = req_builtins[0];

        // count (2)
        req_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {  // count/len
            return look::Value(args.empty() ? 0 : look::look_count(args[0]));  // tek tanım (array_count.h)
        };

        // str/int/float/bool (5-8)
        req_builtins[5] = [](std::vector<look::Value>& args) -> look::Value {
            return look::Value(args.empty() ? "" : args[0].to_string());
        };
        req_builtins[6] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0);
            // to_int(): stoll(to_string(float)) bilimsel-gösterim bug'ı + (int) int32 daralması
            // ikisini de kapatır. Bkz. setup_builtins[6].
            return look::Value(args[0].to_int());
        };
        req_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0.0);
            return look::Value(args[0].to_float());  // to_float(): to_string() round-trip yok
        };
        // bool (8) ve string (9): bind_scalar_builtins (kurulum tablosuyla tek tanım)

        // route() dispatch'te no-op (22)
        req_builtins[22] = [](std::vector<look::Value>&) -> look::Value { return look::Value(); };

        // ── Modül builtin'leri: TEK kaynaktan otomatik wiring ────────────────
        // builtin_names() (look/builtins.h) içindeki her "mod::fn" ismi
        // RuntimeHost'un modül fonksiyonuna bağlanır. Yeni "mod::fn" builtin'i
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

        // timer::after / timer::every / timer::cancel — LOOK 2.
        // Geri çağırma istekten SONRA, zamanlayıcı iş parçacığında çalışır. Orada yeni bir istek
        // gibi kurulur (look_app_dispatch + direct().closure): kendi VM'i, kendi builtin'leri,
        // kurulum global'lerinin temiz hali. Yani bir geri çağırmanın global'e yazdığı şey ne
        // onu kuran isteğe ne sonraki isteklere ulaşır (tests/realtime_isolation_test.sh).
        // Closure iş parçacığı sınırını geçerken klonlanır (yakaladığı değerlerle birlikte).
        {
            auto arm = [](std::vector<look::Value>& args, bool repeat) -> look::Value {
                const char* nm = repeat ? "timer::every" : "timer::after";
                if (args.size() < 2) throw std::runtime_error(std::string(nm) + "() takes 2 arguments");
                if (args[1].type() != look::Value::BYTECODE_FN)
                    throw std::runtime_error(std::string(nm) + "() second argument must be a function");
                const int ms = (int)args[0].to_int();
                look::Value fnv = args[1].clone_for_thread();
                auto cb = [fnv]() {
                    look::WebContext ctx;
                    ctx.method = "__TIMER__";
                    std::ostringstream sink;
                    direct().closure = fnv.as_bytecode_fn().get();
                    try { look_app_dispatch(ctx, sink, std::string()); }
                    catch (const std::exception& ex) {
                        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "timer",
                            std::string("callback error: ") + ex.what());
                    } catch (...) {
                        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "timer", "callback unknown error");
                    }
                    direct().closure = nullptr;
                };
                return look::Value(repeat ? look::TimerManager::instance().every(ms, std::move(cb))
                                          : look::TimerManager::instance().after(ms, std::move(cb)));
            };
            req_builtins[BI("timer::after")] = [arm](std::vector<look::Value>& args) { return arm(args, false); };
            req_builtins[BI("timer::every")] = [arm](std::vector<look::Value>& args) { return arm(args, true); };
            req_builtins[BI("timer::cancel")] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) throw std::runtime_error("timer::cancel() takes 1 argument");
                look::TimerManager::instance().cancel((int)args[0].to_int());
                return look::Value();
            };
            // timer::wait yalnız komut satırında: bir istek zamanlayıcı beklerse iş parçacığını tutar.
            // (Bağlanmasaydı VM "Module 'timer' not loaded" derdi — yanlış neden.)
            req_builtins[BI("timer::wait")] = [](std::vector<look::Value>&) -> look::Value {
                throw std::runtime_error("timer::wait() can be used only on the command line: a web request does not wait for timers");
            };
            // jobs::run — LOOK 2: TEK tanım (RuntimeHost::jobs_run). İşleyici (BYTECODE_FN)
            // vm_closure_runner ile yeni bir istek gibi kendi VM'inde çalışır.
            req_builtins[BI("jobs::run")] = [copy](std::vector<look::Value>& args) -> look::Value {
                return copy->jobs_run(args.empty() ? 5000 : (int)args[0].to_float());
            };
            // ws:: / sse:: — LOOK 2: bağlantı tutamacı üzerindeki işlemler (geri çağırma almayanlar).
            // Denetimler ve hata metinleri eski yorumlayıcıdaki satır içi tanımlarla aynı.
            auto ws_of = [](const look::Value& v, const char* what) {
                if (v.type() != look::Value::WEBSOCKET || !v.as_websocket())
                    throw std::runtime_error(std::string(what));
                return v.as_websocket();
            };
            auto sse_of = [](const look::Value& v, const char* what) {
                if (v.type() != look::Value::SSE_CONN || !v.as_sse())
                    throw std::runtime_error(std::string(what));
                return v.as_sse();
            };
            // ws::on / sse::on — geri çağırma, bağlantının iş parçacığında, zamanlayıcı gibi YENİ BİR
            // İSTEK olarak kurulur (kendi VM'i, global'lerin bildirilen değerleri); hata loglanır,
            // hiçbir şey yeniden çalıştırılmaz.
            auto run_cb = [](const look::Value& fnv, std::vector<look::Value> cb_args, const char* tag, const char* kind) {
                look::WebContext ctx;
                ctx.method = "__EVENT__";
                std::ostringstream sink;
                direct().closure = fnv.as_bytecode_fn().get();
                direct().args    = &cb_args;
                direct().what    = kind;
                try { look_app_dispatch(ctx, sink, std::string()); }
                catch (const std::exception& ex) {
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, tag, std::string(kind) + " error: " + ex.what());
                } catch (...) {
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, tag, std::string(kind) + " unknown error");
                }
                direct().closure = nullptr;
                direct().args    = nullptr;
                direct().what    = nullptr;
            };
            req_builtins[BI("ws::on")] = [ws_of, run_cb](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 3) throw std::runtime_error("ws::on() takes 3 arguments");
                auto conn = ws_of(args[0], "ws::on() first argument must be a websocket");
                if (args[2].type() != look::Value::BYTECODE_FN)
                    throw std::runtime_error("ws::on() third argument must be a function");
                const std::string ev = args[1].to_string();
                look::Value fnv = args[2].clone_for_thread();
                if (ev == "message")
                    conn->on_message = [fnv, run_cb](const std::string& msg) { run_cb(fnv, { look::Value(msg) }, "ws", "ws message callback"); };
                else if (ev == "close")
                    conn->on_close = [fnv, run_cb]() { run_cb(fnv, {}, "ws", "ws close callback"); };
                return look::Value();
            };
            req_builtins[BI("sse::on")] = [sse_of, run_cb](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 3) throw std::runtime_error("sse::on() takes 3 arguments");
                auto conn = sse_of(args[0], "sse::on() first argument must be an SSE connection");
                if (args[2].type() != look::Value::BYTECODE_FN)
                    throw std::runtime_error("sse::on() third argument must be a function");
                look::Value fnv = args[2].clone_for_thread();
                if (args[1].to_string() == "close")
                    conn->on_close_cb = [fnv, run_cb]() { run_cb(fnv, {}, "sse", "sse close callback"); };
                return look::Value();
            };
            req_builtins[BI("ws::send")] = [ws_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 2) throw std::runtime_error("ws::send() takes 2 arguments");
                return look::Value(ws_of(args[0], "ws::send() first argument must be a websocket")->send_text(args[1].to_string()));
            };
            req_builtins[BI("ws::close")] = [ws_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 1) throw std::runtime_error("ws::close() takes 1 argument");
                ws_of(args[0], "ws::close() argument must be a websocket")->close_conn();
                return look::Value();
            };
            req_builtins[BI("ws::broadcast")] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 1) throw std::runtime_error("ws::broadcast() takes 1 argument");
                look::g_ws_registry.broadcast(args[0].to_string());
                return look::Value();
            };
            req_builtins[BI("ws::clients")] = [](std::vector<look::Value>&) -> look::Value {
                return look::Value((int)look::g_ws_registry.count());
            };
            req_builtins[BI("sse::send")] = [sse_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() < 2) throw std::runtime_error("sse::send() takes at least 2 arguments");
                return look::Value(sse_of(args[0], "sse::send() first argument must be an SSE connection")
                                       ->send(args[1].to_string(), args.size() >= 3 ? args[2].to_string() : std::string()));
            };
            req_builtins[BI("sse::close")] = [sse_of](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) throw std::runtime_error("sse::close() takes 1 argument");
                sse_of(args[0], "sse::close() argument must be an SSE connection")->close_conn();
                return look::Value();
            };
            req_builtins[BI("sse::clients")] = [](std::vector<look::Value>&) -> look::Value {
                return look::Value((int)look::g_sse_registry.count());
            };
        }

        // channel()
        req_builtins[BI("channel")] = [](std::vector<look::Value>& args) -> look::Value {
            int buf = args.empty() ? 128 : (args[0].type() == look::Value::INT ? args[0].as_int() : 128);
            if (buf <= 0) buf = (1 << 20);
            return look::Value(std::make_shared<look::LookChannel>(buf));
        };
        // args() — web/CGI'de CLI argümanı yok → boş dizi (CLI-only anlamlı).
        req_builtins[BI("args")] = [](std::vector<look::Value>&) -> look::Value {
            return look::Value(std::make_shared<std::vector<look::Value>>());
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

        // exit()/die() — komut satırıyla aynı semantik (ExitException). Bağlanmazsa
        // builtin slot'u boş kalır → std::bad_function_call (eskiden route KALICI
        // yorumlayıcıya düşüyordu). Web'de exit() zaten nadir.
        {
            auto do_exit = [](std::vector<look::Value>& a) -> look::Value {
                int code = 0;
                if (!a.empty() && a[0].type() == look::Value::INT) code = (int)a[0].as_int();
                throw look::ExitException(code);
            };
            req_builtins[BI("exit")] = do_exit;
            req_builtins[BI("die")]  = do_exit;
        }

        // skaler core builtins — tek tanım (bind_scalar_builtins); kurulum tablosu da onu kullanır
        bind_scalar_builtins(req_builtins);
        // push/pop: builtin_names'de vardı ama req_builtins'de bağlı DEĞİLDİ →
        // route içinde bare push/pop "bad function call" fırlatıp interpreter'a
        // düşürüyordu. In-place mutasyon: args[0] çağıranın $arr'ıyla shared_ptr
        // paylaşır (kopya olsa da aynı vector) → $arr yerinde büyür/küçülür.
        req_builtins[BI("push")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("push() requires array and value");
            args[0].detach();   // LOOK 2: paylaşılan depoyu yerinde değiştirme (değişken yolu PUSH_PATH ile gelir)
            args[0].as_array()->push_back(args[1]);
            return args[0];
        };
        req_builtins[BI("pop")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty() || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("pop() requires array");
            args[0].detach();
            auto a = args[0].as_array();
            if (a->empty()) return look::Value();
            look::Value last = a->back();
            a->pop_back();
            return last;
        };

        if (!local_table) { tl_builtins_gen = cur_gen; tl_builtins_web = &web; }   // cache valid until the next hot-reload
        }   // end if(need_build) — worker-pool builds once per generation; fiber every request

        // before_route closures: route_closures ile aynı yapıda
        std::vector<look::Closure*> before_closures;
        before_closures.reserve(g_http_app.vm_before_routes.size());
        for (auto& val : g_http_app.vm_before_routes)
            if (val.type() == look::Value::BYTECODE_FN)
                before_closures.push_back(val.as_bytecode_fn().get());

        t_dispatch_start = std::chrono::steady_clock::now();
        bool vm_ok = false;
        bool stopped_direct = false;   // zamanlayıcı geri çağırması doğrudan çağrıldı
        try {
            look::VM::SharedState sh;
            sh.routes         = &route_closures;
            sh.builtins       = &req_builtins;
            look::VM vm(sh, output);
            // Kurulum global'leri istekle paylaşılır (harita istek başına kopyalanır, değerler
            // paylaşılır); istek birine yazarsa yazma yolu önce kendi kopyasını alır.
            vm.set_globals(g_http_app.vm_setup_globals);
            vm.set_web_context(&web);
            // LOOK 2: bağlantı işleyicisi (WS/SSE rotası) VM'de — bağlantı işleyiciye ilk argüman olarak gider.
            if (direct().conn.type() == look::Value::WEBSOCKET) vm.set_ws_connection(direct().conn.as_websocket());
            else if (direct().conn.type() == look::Value::SSE_CONN) vm.set_sse_connection(direct().conn.as_sse());

            // LOOK 2 — zamanlayıcı geri çağırması: rota eşleştirme ve before_route yok; closure,
            // yeni bir istek gibi kurulmuş bu VM'de (temiz kurulum değerleri, kendi builtin'leri)
            // doğrudan çağrılır. Hata aşağıdaki ortak yakalayıcıya düşer (log + yeniden çalıştırma yok).
            if (direct().closure) {
                direct().result = vm.call_closure(*direct().closure,
                                direct().args ? *direct().args : std::vector<look::Value>{});
                stopped_direct = true;
            }
            // before_route middleware'leri sırayla çalıştır
            // stop() throw ettiğinde RouteStopException yakalanır → route atlanır
            bool stopped = stopped_direct;
            if (!stopped_direct) for (auto* cl : before_closures) {
                try {
                    vm.call_closure(*cl, {});
                } catch (const look::RouteStopException&) {
                    stopped = true;
                    break;
                }
            }

            if (!stopped) {
                if (g_http_app.script_mode) {
                    // Rotasız betik (PHP gibi): programın tamamı bu istekte, taze global'lerle çalışır.
                    look::VM script_vm(sh, output);
                    script_vm.set_web_context(&web);
                    script_vm.execute(*g_http_app.compiled);
                } else {
                    vm.dispatch_routes(web.method, web.path);
                }
            }
            vm_ok = true;
        } catch (const look::RouteStopException&) {
            vm_ok = true;  // stop() route handler içinde çağrıldı — normal akış
        } catch (const look::ExitException&) {
            // exit()/die() — script'i sonlandırır, VM hatası DEĞİL. Genel catch'e
            // düşerse 500 + hata logu olurdu (yanlış suçlama).
            // O ana dek yazılmış çıktı yanıt olur.
            vm_ok = true;
        } catch (const std::exception& vm_e) {
            // Çalışma zamanı hatası: 500 + log. LOOK 2: yorumlayıcı yok — rota sabitleme de yok.
            //
            // Tarihçe — C9: eskiden ilk hatada route kalıcı olarak yorumlayıcıya sabitleniyordu.
            // Bu güvenlik ağı yıllarca BUG MASKELEDİ — route sessizce yavaş yola düşüp DOĞRU sonuç
            // döndüğü için kimse fark etmiyordu (2026-07-16 turunda bulunan 10 bug'ın çoğu böyle
            // saklanmıştı); log seviyesi ERROR'a çekilmişti, LOOK_VM_STRICT=1 ağı kapatıyordu.
            //
            // 1.0.8 — İŞLEYİCİ ASLA YENİDEN ÇALIŞTIRILMAZ. Yukarıdaki "güvenlik ağı" her hatayı
            // (düz `throw`, veritabanı hatası, tanımsız değişken) "VM hatası" sayıp AYNI isteği
            // yorumlayıcıda baştan çalıştırıyordu: hatadan önce yapılan her yan etki (INSERT,
            // jobs::push, dış HTTP isteği) tek istekte İKİ KEZ gerçekleşiyordu, ve rota kalıcı
            // olarak yavaş yola sabitleniyordu (kullanıcı girdisiyle tetiklenebilir). Ölçüm:
            // "INSERT; sonra hata" yapan rota tek istekte 2 satır yazıyordu.
            // LOOK_VM_STRICT=1 eskiden burada yeniden fırlatıyordu: istisna worker'dan çıkıyor,
            // bağlantılar bırakılmıyor ve worker sayısı kadar hatadan sonra sunucu asılıyordu.
            const std::string what = vm_e.what();
            if (web.status_code == 200) {
                web.status_code = 500;
                web.status_text = "Internal Server Error";
            }
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                (direct().what ? std::string(direct().what) + " error: " : std::string("Dispatch error: ")) + what);
            if (direct().closure) direct().failed = true;
            vm_ok = true;   // yanıt hazır (500) — yorumlayıcıda YENİDEN ÇALIŞTIRMA yok
        }

        // İstek boyunca tembel alınan bağlantılar burada iade edilir — başarılı ve
        // hatalı her iki akışı kapsar. Eksik release, pool_size istek
        // sonrası tüm worker'ların deadlock olmasına neden oluyordu.
        if (!nested) look::release_thread_connections();
    } else {
        // Uygulama yüklenemedi (sıcak yeniden yükleme başarısız: VM derleyemedi/kuramadı). LOOK 2:
        // yorumlayıcı yedeği yok — istek açık bir 500 alır; dosya düzeltilince sonraki yükleme toparlar.
        sl.unlock();
        t_dispatch_start = std::chrono::steady_clock::now();
        web.status_code = 500;
        web.status_text = "Internal Server Error";
        output << "The application could not be loaded; see the server log.";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
            "Request refused: the application is not loaded (the last reload failed; see the error above).");
    }

    auto t_dispatch_end = std::chrono::steady_clock::now();
    auto us_dispatch = std::chrono::duration_cast<std::chrono::microseconds>(t_dispatch_end - t_dispatch_start).count();

    // Süreç-global sayaçlar (runtime::stats için, salt-okunur gözlemlenebilirlik). RuntimeHost istek
    // saymaz → bu global'ler gerçek HTTP durumunu taşır. Clock'lar
    // zaten çağrıldı → latency toplama sıcak yola yeni clock EKLEMEZ (sadece relaxed atomik).
    // zamanlayıcı/bağlantı geri çağırması ve bağlantı işleyicisi bir HTTP isteği değildir: sayaçlara girmez
    if (direct().closure || direct().conn.type() != look::Value::NONE) return;
    look::g_http_request_count().fetch_add(1, std::memory_order_relaxed);
    look::g_latency_us_sum().fetch_add((uint64_t)(us_dispatch < 0 ? 0 : us_dispatch), std::memory_order_relaxed);
    look::g_latency_us_last().store((uint64_t)(us_dispatch < 0 ? 0 : us_dispatch), std::memory_order_relaxed);
    if (web.status_code >= 500)
        look::g_error_5xx_count().fetch_add(1, std::memory_order_relaxed);

    // İlk 200 istekte zamanlama logla — profiling için
    static std::atomic<int> prof_count{0};
    int n = ++prof_count;
    if (n <= 200) {
        auto us_copy = std::chrono::duration_cast<std::chrono::microseconds>(t_copy_end - t_copy_start).count();
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
        std::cerr << "[look-fcgi] File not found: " << script << "\n";
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
        std::cerr << "[look-fcgi --mode http] Setup error: " << e.what() << "\n";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
            std::string("Setup error: ") + e.what() + " — the application does not start.");
        std::exit(1);   // başlamayan uygulama başarı koduyla çıkmasın (eskiden 0 dönüyordu)
    }

    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
        "look-fcgi --mode http port=" + std::to_string(port) + " workers=" + std::to_string(workers));
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Script: " + script.string());
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Apache bypass — use nginx or a direct connection.");

    // Security posture (fail-loud, one-time at startup): no application-level rate
    // limiting AND no declared trusted proxy => this server is likely directly
    // internet-facing with no flood/DoS protection. Behind a reverse proxy the proxy
    // handles rate limiting and slow-client buffering, so LOOK_TRUSTED_PROXY being set
    // is treated as "a proxy is in front" and the warning is suppressed. Never changes
    // behaviour — it only surfaces an insecure-by-omission default so it can't stay silent.
    {
        const RlConfig& rl = rl_config();
        const char* tp = std::getenv("LOOK_TRUSTED_PROXY");
        bool rate_off = (rl.per_rpm <= 0 && rl.global_rpm <= 0);
        bool no_proxy = (!tp || !*tp);
        if (rate_off && no_proxy) {
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "Rate limiting is OFF (LOOK_RATE_LIMIT_RPM unset) and no LOOK_TRUSTED_PROXY is set. "
                "If this server is directly exposed to the internet it has no application-level flood "
                "protection. Set LOOK_RATE_LIMIT_RPM (and consider LOOK_HTTP_MAX_CONNS_IP / "
                "LOOK_BODY_MIN_RATE), or run behind a reverse proxy. See DEPLOYMENT.md.");
        }
    }

    // WebSocket handler — called on a worker thread after 101 upgrade
    auto ws_handler = [](std::shared_ptr<look::WsConnection> conn,
                         const look::HttpRequest& req) {
        look::WebContext web;
        web.method = "WS";
        web.path   = req.path;
        web.query_string = req.query_string;
        web.get_params   = look::WebContext::parse_query(req.query_string);
        web.remote_addr = resolve_client_ip(req);   // WS/SSE: ayni sozlesme (bkz. resolve_client_ip)
        web.is_https    = resolve_is_https(req);

        // LOOK 2: bağlantı işleyicisi sıradan bir istek gibi VM'de dağıtılır.
        // Bağlantı direct().conn ile taşınır ve işleyiciye ilk argüman olarak verilir.
        std::ostringstream sink;
        direct().conn = look::Value(conn);
        try {
            look_app_dispatch(web, sink, std::string());
        } catch (const std::exception& e) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                std::string("WS dispatch error: ") + e.what());
        }
        direct().conn = look::Value();
    };

    // SSE handler — called on a worker thread after SSE upgrade
    auto sse_handler = [](std::shared_ptr<look::SseConnection> conn,
                          const look::HttpRequest& req) {
        look::WebContext web;
        web.method = "SSE";
        web.path   = req.path;
        web.query_string = req.query_string;
        web.get_params   = look::WebContext::parse_query(req.query_string);
        web.remote_addr = resolve_client_ip(req);   // WS/SSE: ayni sozlesme (bkz. resolve_client_ip)
        web.is_https    = resolve_is_https(req);

        // LOOK 2: bağlantı işleyicisi sıradan bir istek gibi VM'de dağıtılır.
        // Bağlantı direct().conn ile taşınır ve işleyiciye ilk argüman olarak verilir.
        std::ostringstream sink;
        direct().conn = look::Value(conn);
        try {
            look_app_dispatch(web, sink, std::string());
        } catch (const std::exception& e) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                std::string("SSE dispatch error: ") + e.what());
        }
        direct().conn = look::Value();
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
                                "smtp", "unsafe recipient name skipped: " + rcpt);
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
            // "started" logu GERÇEK duruma bağlı olmalı. Eskiden koşulsuzdu:
            // bind başarısız olduğunda log şöyle görünüyordu (ölçüldü) —
            //   [ERROR] port 7171 dinlenemedi
            //   [INFO ] IMAP server started on port 7171   ← yanıltıcı
            // Kullanıcı log'a bakıp çalıştığını sanıyordu; sessiz başarısızlık.
            if (smtp_srv->listening()) {
                smtp_thread = std::thread([&smtp_srv] { smtp_srv->run(); });
                look::Logger::instance().log(look::LogLevel::LOG_INFO, "smtp",
                    std::string("SMTP server started on port ") + smtp_port_env);
            } else {
                look::Logger::instance().log(look::LogLevel::LOG_ERROR, "smtp",
                    std::string("SMTP FAILED TO START — port ") + smtp_port_env +
                    " could not be listened on (port busy? permissions? IPv6/IPv4?)");
                smtp_srv.reset();
            }
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
            // start() artık bool döner — "started" logu gerçek duruma bağlı
            // (bkz. SMTP'deki aynı düzeltme: bind hatasından sonra "started" yazıyordu).
            if (imap_srv->start()) {
                look::Logger::instance().log(look::LogLevel::LOG_INFO, "imap",
                    std::string("IMAP server started on port ") + imap_port_env);
            } else {
                look::Logger::instance().log(look::LogLevel::LOG_ERROR, "imap",
                    std::string("IMAP FAILED TO START — port ") + imap_port_env +
                    " could not be listened on (port busy? permissions? IPv6/IPv4?)");
                imap_srv.reset();
            }
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

// look-fcgi --mode http
//
// In HTTP/1.1 mode it listens directly on TCP — bypasses Apache/FastCGI.
// Nginx can optionally sit in front as a reverse proxy.
//
// The existing --mode fcgi (default) does not change at all.
// The same .lk code runs in both modes.

#include "look/num_ops.h"   // abs/floor/ceil/round single definition
#include "look/read_stream.h"   // read_stream — read the file in one go
#include "look/builtins.h"
#include "look/weak_settings.h"   // warnings for weakening settings at startup
#include "look/value_path.h"   // value_push_not_map — error for push onto a dict (single definition)
#include "look/shutdown.h"     // graceful shutdown (SIGTERM/SIGINT)
#include "look/mem_budget.h"   // memory budget: request account + 503 at 90%
#include <optional>
#include "look/route_group.h"
#include "look/host_port.h"   // LOOK_SMTP_PORT / LOOK_IMAP_PORT validation
#include "look/metrics.h"
#include "look/array_count.h"
#include "look/http_server.h"
#include <algorithm>
#include "look/lexer.h"
#include "look/parser.h"
#include "look/jobs_store.h"
#include "look/runtime_host.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/service_error.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/stdlib.h"   // csrf_secret_problem / csrf_request_ok (the default CSRF gate)
#include "look/sse.h"
#include "look/websocket.h"
#include "look/smtp_server.h"
#include "look/imap_server.h"
#include "look/fiber.h"
#include "look/request_scope.h"
#include <set>
#include "look/timer.h"
#include "look/vm_closure_runner.h"   // vm_closure_runner (jobs::run runs the VM handlers)
#include "look/health_path.h"     // built-in health path — request + startup collision check
#include "look/route_params.h"    // route_check_handler — route parameter names are checked at registration
#include "look/struct_types.h"    // struct_check_type_known — field type check after setup

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
// LOOK_RATE_LIMIT_RPM        per-IP  per-minute limit     (0 = disabled)
// LOOK_RATE_LIMIT_BURST      per-IP  instantaneous burst        (default = RPM)
// LOOK_RATE_LIMIT_GLOBAL_RPM global  per-minute limit     (0 = disabled)
// LOOK_RATE_LIMIT_GLOBAL_BURST global instantaneous burst      (default = GLOBAL_RPM)
//
// The global bucket is checked first; if it is full, per-IP is not looked at (saves CPU).
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
        try { prefix = std::stoi(entry.substr(slash + 1)); } catch (...) { continue; }  // bad CIDR -> skip
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

// Resolve the client IP — start from the REAL TCP peer and trust the proxy headers only
// if the connecting IP is in the LOOK_TRUSTED_PROXY list.
//
// OLD BUG: this logic was written CORRECTLY in the rate limiter path but was NOT copied to the places that
// feed request::ip() (ctx.remote_addr); a single line was put there instead:
//     if (req.headers.count("x-forwarded-for")) ctx.remote_addr = <header>;
// Result (--mode http, measured):
//   no header                  -> request::ip() = ""        (the real peer is ignored)
//   X-Forwarded-For: 9.9.9.9    -> request::ip() = "9.9.9.9" (THE CLIENT CHOOSES ITS OWN IP)
//   X-Forwarded-For: a, b       -> the chain is not parsed
//   X-Forwarded-For: <garbage>      -> goes into the log/query/ban list without validation
// IP-based authorization, bans and audit records became untrustworthy.
// Now a single source: all three call sites use this function (a copy drifts).
static std::string trim_sp(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    auto last = s.find_last_not_of(" \t");
    s.erase(last == std::string::npos ? 0 : last + 1);
    return s;
}
// IPv4 (four octets) or IPv6 (hexadecimal, contains ':') form — so that a garbage address in the header is not accepted.
static bool looks_like_ip(const std::string& s) {
    if (s.empty() || s.size() > 45) return false;
    if (s.find(':') != std::string::npos) {
        for (char c : s) if (!(std::isxdigit((unsigned char)c) || c == ':' || c == '.')) return false;
        return true;
    }
    int parts = 0; size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('.', i); if (j == std::string::npos) j = s.size();
        if (j == i || j - i > 3) return false;
        int v = 0; for (size_t k = i; k < j; ++k) { if (!std::isdigit((unsigned char)s[k])) return false; v = v * 10 + (s[k] - '0'); }
        if (v > 255) return false;
        ++parts; i = j + 1;
    }
    return parts == 4;
}
// The RIGHTMOST value of a comma-separated forwarding header — the one added/written by the nearest proxy.
static std::string last_list_value(const std::string& h) {
    auto comma = h.rfind(',');
    return trim_sp(comma == std::string::npos ? h : h.substr(comma + 1));
}

// Client address (behind a trusted proxy): X-Forwarded-For is read from RIGHT to left, trusted proxies are
// skipped, the first untrusted address is the client. Previously X-Real-IP was taken first, then the LEFTMOST entry of XFF:
// behind a proxy that appends (Apache mod_proxy, nginx $proxy_add_x_forwarded_for) the
// client writes the leftmost value (X-Forwarded-For: 6.6.6.6 -> request::ip() = 6.6.6.6; the rate limit, ban and
// audit record were bypassed); Apache does not set X-Real-IP, so whatever the client sent would pass as is. X-Real-IP
// is used only if there is no XFF at all. A malformed entry is not counted as the client: the proxy's own address is returned.
template <typename Req>
static std::string resolve_client_ip(const Req& req) {
    std::string ip = req.remote_addr;              // real TCP peer
    if (!is_trusted_proxy(ip)) return ip;          // untrusted source: do NOT look at the headers
    auto ixff = req.headers.find("x-forwarded-for");
    if (ixff != req.headers.end()) {
        std::vector<std::string> hops;
        std::istringstream ss(ixff->second);
        for (std::string t; std::getline(ss, t, ',');) hops.push_back(trim_sp(t));
        for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
            if (!looks_like_ip(*it)) return ip;
            if (!is_trusted_proxy(*it)) return *it;
        }
        return (!hops.empty() && looks_like_ip(hops.front())) ? hops.front() : ip;   // all are trusted proxies
    }
    auto ireal = req.headers.find("x-real-ip");
    if (ireal != req.headers.end()) {
        std::string r = trim_sp(ireal->second);
        if (looks_like_ip(r)) return r;
    }
    return ip;
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
    std::string p = last_list_value(it->second);   // the nearest proxy's value (the leftmost can be written by the client)
    for (auto& c : p) c = (char)std::tolower((unsigned char)c);
    return p == "https";
}

// The request's host (request::host, the csrf:: Origin check). Same trust rule: X-Forwarded-Host
// only from a trusted proxy — otherwise a client could name any host and pass the Origin check.
template <typename Req>
static std::string resolve_host(const Req& req) {
    if (is_trusted_proxy(req.remote_addr)) {
        auto it = req.headers.find("x-forwarded-host");
        if (it != req.headers.end()) {
            // The rightmost: Apache mod_proxy appends ("client's, real"); the leftmost is written by the client.
            // (nginx does not set it on its own: the configuration MUST write X-Forwarded-Host with $host — documented.)
            std::string h = last_list_value(it->second);
            std::string c = look::WebContext::clean_host(h);
            if (!c.empty()) return c;
        }
    }
    auto it = req.headers.find("host");
    return it == req.headers.end() ? std::string() : look::WebContext::clean_host(it->second);
}

// ── Shared hot-reload state (mirrors WarmApp in fcgi_main) ───────────────────

struct HttpApp {
    // Setup/hot-reload counter — increments on every re-setup. The thread_local dispatch copy in the workers
    // sees it and refreshes itself (see tl_copy in the per-request
    // path). Without the counter the old copy would be used after a hot-reload.
    std::atomic<uint64_t>                    generation{0};

    // ── Module host (RuntimeHost) + program ───────────────────────────────
    // shared_ptr: the generation image (Snap) holds it too — while hot reload sets up the new one, the old one lives
    // until the requests using the old generation finish.
    std::shared_ptr<look::RuntimeHost>       interp;
    std::shared_ptr<look::Program>           program;
    std::ostringstream                       setup_out;

    // ── Bytecode VM (single engine — the app does not start if it cannot be compiled) ──────────────
    std::shared_ptr<look::CompiledProgram>   compiled;

    // VM route registry: filled by running the VM in the setup phase.
    // pattern = "GET:/path/{id}", closure = compiled closure ptr (BYTECODE_FN value)
    struct VmRouteRaw { std::string pattern; look::Value fn; std::vector<look::Value> middlewares; };
    std::vector<VmRouteRaw>                          vm_routes;
    // indicates that the VM is ready for dispatch.
    bool                                     vm_routes_ready = false;
    // A script that registers no routes (like PHP): every request runs the whole program from the start (in the VM).
    bool                                     script_mode = false;
    // before_route() middleware list (registered in the setup phase, runs before dispatch)
    std::vector<look::Value>                 vm_before_routes;
    // Default CSRF check: on if the program uses cookies (session::/csrf::/cookie::);
    // csrf::exempt("/path" | "/prefix/*") rules are registered at setup.
    bool                                     csrf_on = false;
    std::vector<std::string>                 csrf_exempt;
    // Setup VM globals: db_check/json_ok/json_error/admin_check closures + $conn pool key
    std::unordered_map<std::string, look::Value> vm_setup_globals;

    fs::path                                 script_path;
    fs::file_time_type                       mtime;

    // ── Generation image: everything a request uses, immutable ──────────────
    // Under g_http_mutex a request only copies this shared_ptr, releases the lock and from then on reads
    // ONLY from this image. Hot reload publishes a new image; the old one (program,
    // closures, host) is freed when the last request using it finishes.
    // Previously the request released the lock before running the route and continued with raw pointers to g_http_app:
    // if the file changed while the request waited in a query / http::get, the reload freed the old bytecode,
    // and when the request returned the process died with SIGSEGV (ASan: heap-use-after-free in
    // VM::run; in both fiber and thread mode). Holding the lock for the whole request is not a fix: if a suspended
    // fiber holds the read lock while a write lock is awaited on the same thread, the thread deadlocks.
    struct Snap {
        std::shared_ptr<look::RuntimeHost>             interp;
        std::shared_ptr<look::Program>                 program;
        std::shared_ptr<look::CompiledProgram>         compiled;
        std::vector<VmRouteRaw>                        vm_routes;          // owner of the closures
        std::vector<look::Value>                       vm_before_routes;
        std::unordered_map<std::string, look::Value>   vm_setup_globals;
        std::vector<std::string>                       csrf_exempt;
        bool                                           csrf_on = false;
        bool                                           script_mode = false;
        bool                                           vm_routes_ready = false;
        // Route list (statics first) — raw pointers to the closures in vm_routes; valid as long as this image lives.
        std::shared_ptr<std::vector<look::VmRoute>>    route_closures;
    };
    std::shared_ptr<const Snap>                      snap;
};

static HttpApp                  g_http_app;
static std::shared_mutex        g_http_mutex;
static std::atomic<bool>        g_http_ready{false};

// Publish an immutable image from the state left by setup (called under the g_http_mutex WRITE lock).
// A failed load is also published (vm_routes_ready=false -> request 500), same as today's behavior.
static void publish_http_snapshot() {
    auto s = std::make_shared<HttpApp::Snap>();
    s->interp           = g_http_app.interp;
    s->program          = g_http_app.program;
    s->compiled         = g_http_app.compiled;
    s->vm_routes        = g_http_app.vm_routes;
    s->vm_before_routes = g_http_app.vm_before_routes;
    s->vm_setup_globals = g_http_app.vm_setup_globals;
    s->csrf_exempt      = g_http_app.csrf_exempt;
    s->csrf_on          = g_http_app.csrf_on;
    s->script_mode      = g_http_app.script_mode;
    s->vm_routes_ready  = g_http_app.vm_routes_ready;
    // The route list once, at load (previously on every thread, when the generation changed).
    auto rc = std::make_shared<std::vector<look::VmRoute>>();
    rc->reserve(s->vm_routes.size());
    for (auto& r : s->vm_routes) {
        if (r.fn.type() != look::Value::BYTECODE_FN) continue;
        look::VmRoute vr;
        vr.pattern = r.pattern;
        vr.fn      = r.fn.as_bytecode_fn().get();
        for (auto& mw : r.middlewares)
            if (mw.type() == look::Value::BYTECODE_FN) vr.middlewares.push_back(mw.as_bytecode_fn().get());
        rc->push_back(std::move(vr));
    }
    // Static routes (those without {param}) BEFORE the dynamic ones: so that /user/new matches before /user/{id}.
    std::stable_partition(rc->begin(), rc->end(),
        [](const look::VmRoute& r) { return r.pattern.find('{') == std::string::npos; });
    s->route_closures = std::move(rc);
    g_http_app.snap = std::move(s);
}

// The closure to call if this thread is running a timer callback.
// When look_app_dispatch sees this it does not match routes, it calls the closure directly in the request VM.
// Inside a fiber it is FIBER-specific (FiberLocal::http_direct): so that when the handler is suspended in a query
// another fiber on the same thread does not overwrite it. Outside a fiber it is thread-specific.
struct DirectState {
    const look::Closure* closure = nullptr;
    // ... and the arguments to pass it (like a WebSocket message); if absent it is called without arguments.
    const std::vector<look::Value>* args = nullptr;
    // The WebSocket/SSE CONNECTION, if its route handler is running (the WEBSOCKET / SSE_CONN
    // value). The request VM sees this and passes it to the handler as the first argument.
    look::Value conn;
    // Log label of the directly called callback ("message handler" ...): errors are logged under this name.
    const char* what = nullptr;
    // Result and error state of the direct call (the job handler looks at the return value).
    look::Value result;
    bool failed = false;
    // The generation in which the WS/SSE callback's connection was established (program + setup values). If present,
    // dispatch uses this instead of the current generation: the callback is the bytecode of its own program.
    std::shared_ptr<const HttpApp::Snap> gen;
};
// fiber-ok: non-fiber fallback only; inside a fiber direct() returns FiberLocal::http_direct
static thread_local DirectState t_direct_thread;
static DirectState& direct() {
    if (look::Fiber* f = look::Fiber::current()) {
        if (!f->local.http_direct) f->local.http_direct = std::make_shared<DirectState>();
        return *static_cast<DirectState*>(f->local.http_direct.get());
    }
    return t_direct_thread;
}
// Is a dispatch currently in progress on this thread (a nested "new request" runs on a separate thread).
// fiber-ok: non-fiber fallback only; inside a fiber FiberLocal::dispatch_depth
static thread_local int t_dispatch_depth = 0;
static look::Value run_closure_as_request(const look::Value& fnv, std::vector<look::Value> args, const char* what, bool& ok);

// 1.0.8 — Does a route reach a built-in function the VM DOES NOT PROVIDE? (inside a route
// timer::after, jobs::worker ... — the compiler compiles them as `LOAD_GLOBAL "mod::fn"` + a generic CALL,
// in the VM the value is null and the call fails with "Not callable".) Previously these routes only worked
// thanks to the "if an error occurs re-run the same request in the interpreter" net; that net was removed because it ran the
// handler TWICE. The decision is now made BEFORE running, at setup:
// the route's proto, its inner protos and the global functions it calls by name (transitively) are scanned;
// if a "mod::fn" name with no counterpart in the VM globals is loaded the app does not start (in 1.0.8
// the route was pinned to the interpreter from the start). A dynamic call through a variable
// cannot be seen: in that case the request gets a 500.
// Pure (non-request) bare built-ins: bool, string, strlen, abs, max, min, sqrt, strtoupper,
// strtolower, join.
// The REQUEST table and the SETUP table use the same definition. Previously they existed only in the request table:
// the VM setup pass returned an empty value, and a variable computed at top level with `$x = strtoupper("a")`
// was null in routes (correct in the interpreter) — a silent wrong value.
// On the web the LOCATION of an uncaught error is written to the log: " (line 3)" / " (line 3 in lib/x.lk)". The command
// line took this from the VM's last error line (main.cpp); the web log only wrote the message
// ("Dispatch error: boom" — which line?). The object is set up in the same scope as the VM; when the error leaves the VM and
// leaves the scope (while the stack unwinds) it records the location — the VM no longer exists in the catch block.
struct VmErrorPlace {
    const look::VM& vm;
    std::string& out;
    ~VmErrorPlace() {
        if (std::uncaught_exceptions() > 0 && vm.last_error_line() > 0)
            out = " (line " + std::to_string(vm.last_error_line())
                + (vm.last_error_file().empty() ? std::string() : " in " + vm.last_error_file()) + ")";
    }
};

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
    t[BI("string")] = [](std::vector<look::Value>& args) -> look::Value {   // same as str
        return look::Value(args.empty() ? "" : args[0].to_string());
    };
    t[BI("strlen")] = [](std::vector<look::Value>& args) -> look::Value {
        return look::Value(args.empty() ? 0 : (int)args[0].to_string().size());
    };
    t[BI("abs")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.empty()) return look::Value(0);
        return look::num_abs(args[0]);   // look/num_ops.h (INT64_MIN is defined)
    };
    t[BI("max")] = [](std::vector<look::Value>& args) -> look::Value {
        for (auto& x : args) look::value_list_only(x, "max()");   // dict: error (value_path.h)
        return look::num_extreme(args, true);                     // single definition (look/num_ops.h)
    };
    t[BI("min")] = [](std::vector<look::Value>& args) -> look::Value {
        for (auto& x : args) look::value_list_only(x, "min()");
        return look::num_extreme(args, false);
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
    // bare join(arr, sep="") — global builtin (no `use` needed).
    t[BI("join")] = [](std::vector<look::Value>& args) -> look::Value {
        if (args.empty() || args[0].type() != look::Value::ARRAY)
            return look::Value(args.empty() ? std::string() : args[0].to_string());
        look::value_list_only(args[0], "join()");
        std::string sep = args.size() >= 2 ? args[1].to_string() : "";
        std::string result;
        auto& arr = *args[0].as_array();
        for (size_t i = 0; i < arr.size(); ++i) { if (i) result += sep; result += arr[i].to_string(); }
        return look::Value(result);
    };
}

// Does the program use cookies: a session::, csrf:: or cookie:: call (CALL_BUILTIN or a module function
// loaded by name). The rule is one sentence: every app that uses cookies is protected against CSRF.
static bool cookie_api(const std::string& n) {
    return n.rfind("session::", 0) == 0 || n.rfind("csrf::", 0) == 0 || n.rfind("cookie::", 0) == 0;
}
static bool proto_uses_cookies(const look::FunctionProto* p, std::unordered_set<const look::FunctionProto*>& seen) {
    if (!p || !seen.insert(p).second) return false;
    const auto& names = look::builtin_names();
    for (size_t i = 0; i < p->code.size(); ++i) {
        const auto& ins = p->code[i];
        if (ins.op == look::OpCode::CALL_BUILTIN) {
            uint32_t bidx = ins.b;
            if (i + 1 < p->code.size() && p->code[i + 1].op == look::OpCode::NOP)
                bidx = look::Instruction::builtin_index(ins, p->code[i + 1]);
            if (bidx < names.size() && cookie_api(names[bidx])) return true;
        } else if (ins.op == look::OpCode::LOAD_GLOBAL) {
            const size_t ni = ins.bx();
            if (ni < p->constants.size() && p->constants[ni].type() == look::Value::STRING &&
                cookie_api(p->constants[ni].str_ref())) return true;
        }
    }
    for (const auto& n : p->nested)
        if (proto_uses_cookies(n.get(), seen)) return true;
    return false;
}
// csrf::exempt rule: "/path" (with {name} parts like route()) or "/prefix/*".
static bool csrf_exempt_match(const std::string& rule, const std::string& path) {
    if (!rule.empty() && rule.back() == '*') return path.rfind(rule.substr(0, rule.size() - 1), 0) == 0;
    size_t i = 0, j = 0;
    while (i < rule.size() && j < path.size()) {
        if (rule[i] == '{') {
            auto close = rule.find('}', i);
            if (close == std::string::npos) return false;
            size_t start = j;
            while (j < path.size() && path[j] != '/') ++j;
            if (j == start) return false;
            i = close + 1;
            continue;
        }
        if (rule[i] != path[j]) return false;
        ++i; ++j;
    }
    return i == rule.size() && j == path.size();
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

// Startup info lines ([BYTECODE] ..., "VM dispatch ready") — meaningful for a long-lived server.
// lk-cgi is a new process on every request: stderr goes to Apache's error log -> there it would
// be noise on every request (the old lk-cgi wrote nothing to stderr on a successful request). CGI turns it off;
// warnings and errors are always written.
static bool g_http_quiet_startup = false;
void look_http_engine_set_quiet(bool quiet) { g_http_quiet_startup = quiet; }
static void startup_note(const std::string& stderr_line, const std::string& log_line = "") {
    if (g_http_quiet_startup) return;
    std::cerr << stderr_line << "\n";
    if (!log_line.empty()) look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", log_line);
}

// Single engine: if the program cannot be compiled the app DOES NOT START (previously the whole app silently
// fell to a ~40x slower interpreter; LOOK_VM_STRICT was only a way to reject this — now
// this is the only behavior). A compile error is the program's error (missing file, unknown module, limit
// exceeded) — the message is passed on as it is; the "the VM cannot run it" framing blamed the engine.
// Throws an exception: at startup the caller writes the message and exits with code 1; on hot reload
// the error is logged, the server does not shut down (the app is not ready -> requests get 500, the next load
// recovers once the file is fixed).
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
    // The database connections taken by setup (top-level code) return to the pool when setup ends.
    // Previously they stayed with the setup thread: a top-level db::exec (like CREATE TABLE)
    // held a connection from the pool for the lifetime of the process — with LOOK_DB_POOL_SIZE=1 every DB
    // request waited forever (a regression that came with v2's lazy pool). Also on exit with an error.
    struct SetupConnRelease { ~SetupConnRelease() { look::release_thread_connections(); } } setup_conn_release;
    // However setup ends (success / error), a new immutable image is published for requests.
    struct SnapPublish { ~SnapPublish() { publish_http_snapshot(); } } snap_publish;

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

    // Single engine: top-level code (setup) runs ONLY in the VM, once. This object does NOT RUN
    // LOOK code — it is only the store of the built-in modules (ModuleHost): module functions are taken
    // from it, web modules are bound to the setup context (setup_ctx), it carries the setup-mode flag.
    // (Previously top level ran here first in the interpreter, then the VM replayed the same code from the
    // record: two passes, two engines.)
    auto interp = std::make_unique<look::RuntimeHost>(g_http_app.setup_out);
    interp->set_file(script.string());
    interp->set_web_context(&setup_ctx);
    // Modules are opened AFTER compilation (below, CompiledProgram::use_modules).
    g_http_app.program     = std::move(program);
    g_http_app.interp      = std::move(interp);
    g_http_app.script_path = script;

    // ── Bytecode compile — single engine, no fallback ───────────────────────
    // LOOK_BYTECODE=0 used to run the whole app in the interpreter; it no longer has any meaning.
    {
        static bool warned = false;
        const char* e = std::getenv("LOOK_BYTECODE");
        if (e && std::string(e) == "0" && !warned) {
            warned = true;
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "LOOK_BYTECODE=0 is ignored: LOOK runs only on the VM.");
        }
    }

    {
        try {
            startup_note("[BYTECODE] Compiling...");
            auto compiled = std::make_unique<look::CompiledProgram>(
                look::Compiler::compile(*g_http_app.program,
                                        g_http_app.script_path.parent_path().string(), g_http_app.script_path.string()));
            // Only the stdlib modules the program `use`s are opened (those of included files too —
            // the compiler collects them from the `use` lines it already read; the same list as the CLI). The slot of a
            // module that is not `use`d stays empty -> the VM gives "Module 'x' not loaded." A package module
            // (~/.look/modules) is not stdlib: the compiler compiles it.
            for (const auto& m : compiled->use_modules) g_http_app.interp->load_stdlib_module(m);
            g_http_app.compiled     = std::move(compiled);
            startup_note("[BYTECODE] OK — VM mode active", "Bytecode compile OK — VM mode active");
        } catch (const std::exception& e) {
            refuse_start(e.what());
        } catch (...) {
            refuse_start("the program could not be compiled");
        }

    // ── VM Setup Phase: run the bytecode, register the routes ────────────────
    g_http_app.vm_routes.clear();
    g_http_app.vm_before_routes.clear();
    g_http_app.csrf_on = false;
    g_http_app.csrf_exempt.clear();
    g_http_app.vm_routes_ready = false;
    g_http_app.script_mode     = false;
    if (g_http_app.compiled) {
        std::string setup_err_place;   // line of the setup error (VmErrorPlace)
        try {
            // Setup builtins: route() (index 22) and the basic data functions
            // The other builtins are not called at setup (db::query etc. need an HTTP context)
            auto vm_routes_ptr        = &g_http_app.vm_routes;
            auto vm_before_routes_ptr = &g_http_app.vm_before_routes;
            std::vector<look::BuiltinFn> setup_builtins(look::builtin_names().size());

            // print/write — for setup output (0, 1)
            auto* setup_out_ptr = &g_http_app.setup_out;
            setup_builtins[0] = [setup_out_ptr](std::vector<look::Value>& args) -> look::Value {
                for (auto& a : args) *setup_out_ptr << a.to_string();
                return look::Value();
            };
            setup_builtins[1] = setup_builtins[0];

            // count (2)
            setup_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {  // count/len
                return look::Value(args.empty() ? 0 : look::look_count(args[0]));  // single definition (array_count.h)
            };

            // push (3) — for $routes_list pushes in setup
            setup_builtins[3] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                look::value_push_not_map(args[0], "push()");
                args[0].detach();   // do not modify the shared store in place (the variable path comes via PUSH_PATH)
                args[0].as_array()->push_back(args[1]);
                return look::Value();
            };
            // pop (4)
            setup_builtins[4] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty() || args[0].type() != look::Value::ARRAY)
                    return look::Value();
                look::value_list_only(args[0], "pop()");
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
                // USE to_int() — stoll(to_string(float)) is BROKEN: to_string turns a large float
                // into scientific notation ("1e+06"), stoll stops at 'e' and returns 1 ->
                // int(float>~1e6) is silently wrong. to_int() casts a FLOAT directly to (int64_t)
                // and parses a STRING with the same stoll.
                return look::look_explicit_int(args[0]);   // out-of-range numeric text -> error (Go ErrRange)
            };
            // float (7)
            setup_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.empty()) return look::Value(0.0);
                // to_float(): handle to_string() round-trip as in int() (FLOAT directly,
                // no precision loss from a string round-trip).
                return look::look_explicit_float(args[0]);
            };

            bind_scalar_builtins(setup_builtins);   // strlen, abs, max, min, sqrt, strtoupper, strtolower, join — same definition as the request table

            // before_route() — register the middleware closure at setup. The index is FROM THE NAME
            // (builtin_index) — a hard-coded number drifted when the builtin list grew.
            setup_builtins[(size_t)look::builtin_index("before_route")] =
                [vm_before_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                if (!args.empty() && args[0].type() == look::Value::BYTECODE_FN)
                    vm_before_routes_ptr->push_back(args[0]);
                return look::Value();
            };

            // stop() — a no-op at setup (only meaningful at dispatch)
            setup_builtins[(size_t)look::builtin_index("stop")] =
                [](std::vector<look::Value>&) -> look::Value {
                return look::Value();
            };

            // route::group(prefix, [mw], fn) — push the group context, call the closure (invoke bridges
            // to the active setup VM), pop. route() calls inside inherit the prefix+mw.
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

            // route() — index 22: register pattern + closure
            setup_builtins[22] = [vm_routes_ptr](std::vector<look::Value>& args) -> look::Value {
                // route("GET", "/path", fn) — 3-arg form
                // route("GET", "/path", [mw...], fn) — 4-arg form
                // route("404", fn) — 2-arg form
                if (args.size() >= 4) {
                    // 4-arg: method, pattern, middlewares, fn
                    std::string method  = args[0].to_string();
                    std::string pattern = look::route_group_prefix() + args[1].to_string();
                    // group middlewares (outer->inner) FIRST, then the route's own mw's
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

            // CRITICAL: overrides are indexed BY NAME (builtin_index). They used to be hard-coded numbers;
            // when an entry was added to builtin_names() the indexes drifted (silently the wrong function).
            // db::connect and the other module functions are bound to the REAL module
            // function in the general loop below (setup is a single pass: the pool is opened once).
            auto SBI = [](const char* n) { return (size_t)look::builtin_index(n); };
            setup_builtins[SBI("channel")] = [](std::vector<look::Value>& args) -> look::Value {
                return look::Value(look::make_channel(args));   // value.h: same rule as the command line
            };
            setup_builtins[SBI("args")] = [](std::vector<look::Value>&) -> look::Value {
                return look::Value(std::make_shared<look::ValueArray>());
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
            // app:: — service registration is called at setup (app::set("db", $conn)).
            // The index is taken by name (builtin_index) — append-only, no misalignment.
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

            // ── Remaining built-ins: REAL module functions (setup is a SINGLE pass) ─────────
            // Top level runs only here, in the VM, once: the side effects of module functions
            // (db::exec, file::put, cache::set, http::get ...) happen once, their returns are real.
            // Previously the interpreter pass ran them and the VM pass replayed the results from the
            // record (1.0.9) — two passes, two engines; neither exists any more.
            {
                const auto& bnames = look::builtin_names();
                look::RuntimeHost* host = g_http_app.interp.get();
                for (size_t i = 0; i < setup_builtins.size() && i < bnames.size(); ++i) {
                    if (setup_builtins[i]) continue;           // specially bound above
                    const std::string& name = bnames[i];
                    // A timer set at top level: the callback is a VM closure, and on every fire it runs in a VM
                    // set up like a new request (the same path as one set in a route).
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
                    // jobs::run — runs the handlers (VM handlers via vm_closure_runner).
                    if (name == "jobs::run") {
                        setup_builtins[i] = [host](std::vector<look::Value>& args) -> look::Value {
                            return host->jobs_run(args.empty() ? 5000 : (int)args[0].to_float());
                        };
                        continue;
                    }
                    auto colon = name.find("::");
                    // A module that is not `use`d: the slot stays empty, the VM says "Module 'x' not loaded."
                    if (colon != std::string::npos && host->module_not_used(name.substr(0, colon))) continue;
                    if (colon != std::string::npos) {
                        auto f = host->get_module_fn(name.substr(0, colon), name.substr(colon + 2));
                        if (f) {
                            // jobs::worker: the handler is called on other threads -> copy the closure.
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
                    // Built-in with no meaning at setup (ws::on, context that exists during a request ...):
                    // explicit error — it used to silently return null.
                    setup_builtins[i] = [name](std::vector<look::Value>&) -> look::Value {
                        throw look::LookVmError(name + "() cannot be used at the top level of a web application");
                    };
                }
            }
            // csrf::exempt("/path" | "/prefix/*") — exemption registered at setup (the module version errors in a request).
            {
                auto* ex = &g_http_app.csrf_exempt;
                setup_builtins[(size_t)look::builtin_index("csrf::exempt")] =
                    [ex](std::vector<look::Value>& args) -> look::Value {
                    if (args.size() != 1 || args[0].type() != look::Value::STRING || args[0].to_string().empty() ||
                        args[0].to_string()[0] != '/')
                        throw std::runtime_error("csrf::exempt(path) takes one path that starts with /, e.g. "
                            "csrf::exempt(\"/webhooks/stripe\") or csrf::exempt(\"/api/*\")");
                    ex->push_back(args[0].to_string());
                    return look::Value();
                };
            }
            // so that jobs::run, if called at setup, can run the VM handlers (set up BEFORE setup).
            look::vm_closure_runner() = run_closure_as_request;

            look::VM::SharedState setup_sh;
            setup_sh.builtins = &setup_builtins;
            std::ostringstream vm_setup_out;
            look::VM setup_vm(setup_sh, vm_setup_out);
            VmErrorPlace setup_place{setup_vm, setup_err_place};
            // Setup is in progress: modules like db::query see this via ModuleHost::is_setup_mode.
            struct SetupMode { look::RuntimeHost* h; SetupMode(look::RuntimeHost* x) : h(x) { h->set_setup_mode(true); }
                               ~SetupMode() { h->set_setup_mode(false); } } setup_mode{g_http_app.interp.get()};
            setup_vm.execute(*g_http_app.compiled);

            // Keep the setup globals: named fn closures like db_check/json_ok/json_error +
            // $conn pool key — will be given to the dispatch VM with set_globals()
            g_http_app.vm_setup_globals = setup_vm.get_globals();

            // Default CSRF check: on if the program uses cookies. If the key is missing the app DOES NOT
            // START (Go model: a missing resource is a reasoned error at startup; not a random 403 while running).
            {
                std::unordered_set<const look::FunctionProto*> seen;
                g_http_app.csrf_on = proto_uses_cookies(g_http_app.compiled->main_proto.get(), seen);
                for (const auto& [k, v] : g_http_app.vm_setup_globals)
                    if (!g_http_app.csrf_on && v.type() == look::Value::BYTECODE_FN)
                        if (auto cl = v.as_bytecode_fn()) g_http_app.csrf_on = proto_uses_cookies(cl->proto.get(), seen);
                if (g_http_app.csrf_on) {
                    std::string problem = look::csrf_secret_problem();
                    if (!problem.empty()) throw std::runtime_error(problem);
                }
                startup_note(g_http_app.csrf_on
                    ? "CSRF protection: on (the program uses cookies; " + std::to_string(g_http_app.csrf_exempt.size()) + " exempt path(s))"
                    : "CSRF protection: off (the program does not use cookies)");
            }

            // All files are loaded, all structs are known -> check the field type names NOW.
            // A typo (`strng`) gives an error when the app STARTS; otherwise it would only show up in production
            // when the route that builds that struct (perhaps rarely called) runs. The definitions
            // are held in the VM globals under "__sdef:<name>" as [field, default, type] triples.
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

            // runtime::stats()["routes"]. OLD BUG (since 4b): the count was
            // read from the interpreter's route registry; when setup moved to the VM that registry stayed empty and stats said 0 routes on the web.
            g_http_app.interp->set_route_count((int)g_http_app.vm_routes.size());
            if (!g_http_app.vm_routes.empty()) {
                g_http_app.vm_routes_ready = true;
                // Single engine: if a route or before_route reaches a module function that DOES NOT EXIST
                // the app DOES NOT START. Every module function exists in the VM (`lk --vm-report`, the CI
                // gate); a `mod::fn` name the VM does not recognize is a typo. Previously such a route
                // was silently moved to the interpreter and the error only appeared when the first request hit that route.
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
                        // Exception — NOT std::exit: on first startup the caller writes it and exits with code 1; on hot
                        // reload the server stays up (requests get 500); lk-cgi writes an error
                        // response. OLD BUG: exit(1) on reload killed the whole server.
                        g_http_app.vm_routes_ready = false;
                        throw std::runtime_error("route " + g_http_app.vm_routes[i].pattern + " calls " + which +
                            "(), which does not exist — check the spelling of the module and the function.");
                    }
                }
                startup_note("[BYTECODE] VM routes: " + std::to_string(g_http_app.vm_routes.size()) + " registered",
                             "VM dispatch ready — " + std::to_string(g_http_app.vm_routes.size()) + " route");
            } else {
                // A script that registers no routes (like PHP): every request runs the whole program from the start in the VM.
                g_http_app.vm_routes_ready = true;
                g_http_app.script_mode     = true;
                startup_note("[BYTECODE] no routes — each request runs the whole script on the VM");
            }
        } catch (const std::exception& e) {
            // Setup runs only in the VM: the error here is the program's OWN error (missing argument,
            // throw, a module that is not `use`d ...), not "the VM cannot run this program" — the message is passed
            // on as it is (the caller writes "Setup error: ...", the app does not start).
            g_http_app.vm_routes_ready = false;
            throw std::runtime_error(e.what() + setup_err_place);
        }
    }
    }
    g_http_app.mtime       = fs::last_write_time(script);

    // Setup/hot-reload completed -> the thread_local dispatch copies in the workers
    // will see this increment and rebuild themselves (so they do not stay hooked to the old interp).
    g_http_app.generation.fetch_add(1, std::memory_order_release);

    g_http_ready = true;
}

// ── Shared application engine API ─────────────────────────────────────────
// FCGI mode (fcgi_main.cpp) and HTTP mode share the same setup + dispatch engine
// — VM dispatch lives in ONE place. (FCGI used to be
// stuck with the interpreter; now both modes use the bytecode VM.)
//
// NOTE: the engine holds a single script. In FCGI, if SCRIPT_FILENAME changes
// from request to request, a full re-setup happens on every change — with
// one app per service (the production model) this is never triggered.

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

// Dispatch model (1.0.20): on Linux the DEFAULT is fiber — waiting for a connection does not hold a worker.
// The old model (one worker per connection) is selected only with LOOK_FIBER_DISPATCH=0. In that model
// as many open connections as workers (including idle browser connections) would lock the server
// until the timeout expired. There is no fiber dispatch outside Linux: only "1" enables it.
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

    // Remote addr — request::ip() reads this. It starts from the real peer and trusts
    // proxy headers only behind a trusted proxy (see
    // resolve_client_ip: previously the header was accepted UNCONDITIONALLY here).
    ctx.remote_addr = resolve_client_ip(req);
    ctx.is_https    = resolve_is_https(req);
    ctx.host        = resolve_host(req);

    // Body parsing — urlencoded AND multipart, from a single source (web.cpp).
    // OLD BUG: only the urlencoded branch existed here, multipart did not at all ->
    // `request::file()` worked in FastCGI, but in `--mode http` gave the "requires
    // multipart/form-data request" error. The same app
    // behaved differently depending on the mode (a mode-specific gap); the README listed it as a "known limit",
    // but it was in fact a forgotten branch.
    // The method gate was also removed: the FastCGI side never checked the method,
    // so PUT/PATCH bodies behaved differently in the two modes.
    if (!req.body.empty()) ctx.parse_post_body();

    return ctx;
}

// ── Request handler called by HttpServer worker threads ───────────────────────

// Forward declaration — the definition comes after http_handler (shared engine)
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path);

// Runs a VM closure LIKE A NEW REQUEST: its own VM, its own builtins,
// the declared values of the globals (for timers and job handlers registered at top level).
// The error is logged (what + " error: ..."), ok=false is returned; nothing is re-run.
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
    // If a request is already being dispatched on this thread (e.g. a route called jobs::run), the nested
    // dispatch is handled in look_app_dispatch: the outer request's caches and database
    // connections are preserved. (Spawning a separate thread would exhaust the pool and deadlock when
    // every consumer holds a connection — tests/jobs_concurrent_test.sh.)
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
        // Start from the real TCP peer; trust proxy headers only if the connecting IP
        // is in the LOOK_TRUSTED_PROXY list. This logic was CORRECT here but
        // was not copied to the request::ip() path and that copy drifted — now both
        // use the single source (resolve_client_ip).
        std::string client_ip = resolve_client_ip(req);
        const auto& rlc = rl_config();

        // Global limit — check first, if full do not look at per-IP.
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

    // Dispatch — shared engine (FCGI mode uses the same function)
    look::WebContext web = make_web_ctx(req);
    std::ostringstream output;
    look_app_dispatch(web, output, req.path);

    // Build response
    resp.status_code = web.status_code;
    resp.status_text = web.status_text;
    for (auto& [k, v] : web.headers_out) resp.headers[k] = v;
    resp.set_cookies = web.set_cookies_out;   // each cookie on its own Set-Cookie line
    resp.body = web.response_body.empty() ? output.str() : web.response_body;

    // Content-Type default
    if (resp.headers.find("Content-Type") == resp.headers.end())
        resp.headers["Content-Type"] = "text/html; charset=utf-8";
}

// Shared request engine — the web ctx must be prepared; dispatches with the VM,
// writes the output to output/web.response_body.
void look_app_dispatch(look::WebContext& web, std::ostringstream& output,
                       const std::string& prof_path) {
    // NESTED dispatch: while a request is in progress a VM closure runs as a "new request"
    // on the same thread (a route called jobs::run -> job handler). The outer request's thread-specific
    // state is preserved: the builtin table is set up locally (the cache is not touched), the database
    // connections of the outer one are used (not re-acquired/released — would exhaust the pool and
    // deadlock), on exit the copy's output and context are bound back to the outer request.
    // In fiber mode the depth and the outer context belong to the FIBER: one thread suspends and resumes many requests
    // as fibers in turn; a thread_local counter took another suspended request
    // for the "outer request" (see FiberLocal::dispatch_depth).
    // fiber-ok: non-fiber fallback only; inside a fiber FiberLocal::dispatch_web/out
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

    // This request's generation: only the image is copied under the lock; until the request ends the image (and its
    // program, closures, host) lives — see HttpApp::Snap.
    std::shared_ptr<const HttpApp::Snap> snap;
    if (direct().closure && direct().gen)
        // The callback of an open WS/SSE connection: the connection's generation. Previously the current generation
        // was taken -> after a hot reload the old callback ran mixed with the new program's values
        // and functions (tests/hot_reload_ws_callback_test.sh).
        snap = direct().gen;
    else { std::shared_lock<std::shared_mutex> sl(g_http_mutex); snap = g_http_app.snap; }
    // vm_routes_ready: the native function wiring must be complete
    // for the VM to use dispatch_routes() (Phase 16.5 Step 5.5)
    bool use_bc = snap && snap->compiled && snap->vm_routes_ready;   // vm_routes_ready is false on a failed load

    auto t_copy_start    = std::chrono::steady_clock::now();
    auto t_copy_end      = t_copy_start;
    auto t_dispatch_start = t_copy_start;

    if (use_bc) {
        // ── VM path ───────────────────────────────────────────────────────────
        // ── Dispatch copy: ONCE per worker thread (NOT per request) ────
        // Measurement (docker, 2 workers): make_dispatch_copy() was 30µs on every request (app without
        // globals) — 2.5x more expensive than RUNNING the route (12µs); in a global-heavy app it was 63µs and
        // throughput dropped 21.2k->12.8k (even though the route was THE SAME). Cause:
        // make_dispatch_copy -> make_unique<RuntimeHost>() rebuilds the WHOLE stdlib (hundreds of
        // std::function) on every request + a deep clone of globals_.
        // PHP-FPM does not pay such a cost per request — this was the real obstacle in front of the goal of passing PHP,
        // not the engine speed.
        //
        // Safety: each worker handles a single request at a time -> a thread_local copy is safe.
        // On hot-reload the generation increases -> the copy is refreshed.
        // ISOLATION: RuntimeHost holds no LOOK global (the VM takes its own globals on every request
        // from snap->vm_setup_globals) -> a clone is unnecessary.
        t_copy_start = std::chrono::steady_clock::now();
        // fiber-ok: generation-fixed dispatch copy; request-specific context is bound on every request, in fiber mode the request table is built per request (tests/fiber_request_isolation_test.sh measures it across suspension)
        // The caches depend on whichever image they were built from (tl_*_of). shared_ptr: this request
        // holds what it uses — if another request on the same thread moves to the new generation and
        // rebuilds the cache, this suspended request keeps using the old copy (previously it was
        // rebuilt in place: the suspended request returned to a freed host/table).
        static thread_local std::shared_ptr<look::RuntimeHost> tl_copy;
        // The image itself is held too: if only the address were stored, the old image would be freed and when the new one was allocated at the same
        // address the old copy would be taken as "current" (ABA).
        static thread_local std::shared_ptr<const HttpApp::Snap> tl_copy_of;   // fiber-ok: generation key
        if (!tl_copy || tl_copy_of != snap) {
            tl_copy = std::shared_ptr<look::RuntimeHost>(snap->interp->make_dispatch_copy());
            tl_copy->bind_web_to_current_request();   // web modules once; the context at call time
            tl_copy_of = snap;
        }
        std::shared_ptr<look::RuntimeHost> copy_hold = tl_copy;
        look::RuntimeHost* copy = copy_hold.get();
        t_copy_end = std::chrono::steady_clock::now();
        copy->set_output(output);
        // Request handle: modules and print read this request's context/output from here (the fiber's
        // own field). When the scope ends the previous value comes back — nested dispatch does not break the outer request.
        look::RequestScope req_scope(&web, &output);
        if (nested && outer_web && outer_out)
            depth_guard.restore = [copy, outer_out]() { copy->set_output(*outer_out); };

        // The route list is built once with the image (publish_http_snapshot); raw pointers to the closures
        // are valid as long as this image lives (snap is held until the end of the request).
        std::vector<look::VmRoute>& route_closures = *snap->route_closures;

        // Builtin table: module functions are taken from the dispatch copy (RuntimeHost); the copy binds the web
        // modules to the request at call time (bind_web_to_current_request).
        // fiber-ok: no entry in the table is request-specific (context/output come from the request handle, per fiber) — tests/fiber_request_isolation_test.sh measures it across suspension
        static thread_local std::shared_ptr<std::vector<look::BuiltinFn>> tl_req_builtins;
        static thread_local std::shared_ptr<const HttpApp::Snap> tl_builtins_of;   // fiber-ok: generation key (no ABA)
        // The table is now built once per thread in fiber mode too: nothing request-specific
        // is captured — print reads the output, web modules read the context at call time from the request handle
        // (look/request_scope.h, the fiber's own field). Previously in fiber mode a ~190-entry
        // table was rebuilt on every request (measured: the largest cost of the request, the VM itself 2%).
        // Nested dispatch (timer/WS callback) does not touch the outer request's table: it has its own table.
        std::vector<look::BuiltinFn> nested_req_builtins;
        const bool local_table = nested;
        const bool need_build = local_table || !tl_req_builtins || (tl_builtins_of != snap);
        // New generation: a NEW table (suspended requests keep the old one — it is not rebuilt in place).
        if (need_build && !local_table) tl_req_builtins = std::make_shared<std::vector<look::BuiltinFn>>();
        std::shared_ptr<std::vector<look::BuiltinFn>> builtins_hold = local_table ? nullptr : tl_req_builtins;
        std::vector<look::BuiltinFn>& req_builtins = local_table ? nested_req_builtins : *builtins_hold;
        if (need_build) {
        req_builtins.assign(look::builtin_names().size(), look::BuiltinFn{});

        // print/write (0, 1): the output is the stream of the request at call time
        req_builtins[0] = [](std::vector<look::Value>& args) -> look::Value {
            std::ostream* out = look::current_request_out();
            if (!out) throw std::runtime_error("print: no output here (this runs outside a request)");
            for (auto& a : args) *out << a.to_string();
            return look::Value();
        };
        req_builtins[1] = req_builtins[0];

        // count (2)
        req_builtins[2] = [](std::vector<look::Value>& args) -> look::Value {  // count/len
            return look::Value(args.empty() ? 0 : look::look_count(args[0]));  // single definition (array_count.h)
        };

        // str/int/float/bool (5-8)
        req_builtins[5] = [](std::vector<look::Value>& args) -> look::Value {
            return look::Value(args.empty() ? "" : args[0].to_string());
        };
        req_builtins[6] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0);
            // to_int(): closes both the stoll(to_string(float)) scientific-notation bug + the (int) int32 narrowing.
            // See setup_builtins[6].
            return look::look_explicit_int(args[0]);   // out-of-range numeric text -> error (Go ErrRange)
        };
        req_builtins[7] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value(0.0);
            return look::look_explicit_float(args[0]);  // out of range -> error
        };
        // bool (8) and string (9): bind_scalar_builtins (single definition with the setup table)

        // route() is a no-op at dispatch (22)
        req_builtins[22] = [](std::vector<look::Value>&) -> look::Value { return look::Value(); };

        // ── Module builtins: automatic wiring from a SINGLE source ────────────────
        // Every "mod::fn" name in builtin_names() (look/builtins.h) is bound
        // to RuntimeHost's module function. To add a new "mod::fn" builtin
        // it is ENOUGH to add it to the builtins.cpp list —
        // no manual sync (the root of the strlen bug was a manual-sync gap).
        {
            const auto& names = look::builtin_names();
            for (size_t idx = 0; idx < names.size(); ++idx) {
                auto pos = names[idx].find("::");
                if (pos == std::string::npos) continue;  // special name — bound by hand
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

        // CRITICAL: the manual override indexes are now resolved BY NAME (builtin_index).
        // They used to be hard-coded numbers; when an entry was added to the builtin_names() list
        // all indexes drifted and these overrides overwrote the WRONG builtin
        // (e.g. the before_route no-op overwrote response::json and JSON APIs
        // returned empty). The name-based resolution closes this class of sync gaps.
        auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };

        // timer::after / timer::every / timer::cancel.
        // The callback runs AFTER the request, on the timer thread. There it is set up like a new request
        // (look_app_dispatch + direct().closure): its own VM, its own builtins,
        // a clean copy of the setup globals. So what a callback writes to a global reaches neither
        // the request that set it up nor later requests (tests/realtime_isolation_test.sh).
        // The closure is cloned when it crosses the thread boundary (together with the values it captured).
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
            // timer::wait only on the command line: a request that waits on a timer holds the thread.
            // (If it were not bound the VM would say "Module 'timer' not loaded" — the wrong reason.)
            req_builtins[BI("timer::wait")] = [](std::vector<look::Value>&) -> look::Value {
                throw std::runtime_error("timer::wait() can be used only on the command line: a web request does not wait for timers");
            };
            // jobs::run — SINGLE definition (RuntimeHost::jobs_run). The handler (BYTECODE_FN)
            // runs in its own VM like a new request via vm_closure_runner.
            req_builtins[BI("jobs::run")] = [copy](std::vector<look::Value>& args) -> look::Value {
                return copy->jobs_run(args.empty() ? 5000 : (int)args[0].to_float());
            };
            // ws:: / sse:: — operations on the connection handle (those that take no callback).
            // The checks and error texts are the same as the old inline definitions in the interpreter.
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
            // ws::on / sse::on — the callback is set up as a NEW REQUEST on the connection's thread, like a
            // timer (its own VM, the declared values of the globals); the error is logged,
            // nothing is re-run.
            // handshake: in the ws callback a copy of the handshake request — request::get freezes the ws address's
            // parameters (same rule as v1); an empty request for other events.
            // Storage format: the handshake request's REQUEST fields in a single string (length-prefixed).
            // Keeping a full WebContext per open connection (~10 strings + ~9 maps, even when empty)
            // cost ~0.7 KB per connection (700k WS: ~618 MB -> ~1156 MB). The response fields and
            // uploaded files belong to the handshake; the callback's response goes nowhere anyway.
            auto pack_req = [](const look::WebContext& w) {
                std::string b;
                auto s = [&b](const std::string& v) { uint32_t n = (uint32_t)v.size(); b.append((const char*)&n, 4); b.append(v); };
                auto m = [&b, &s](const std::map<std::string, std::string>& mp) {
                    uint32_t n = (uint32_t)mp.size(); b.append((const char*)&n, 4);
                    for (const auto& [k, v] : mp) { s(k); s(v); }
                };
                s(w.method); s(w.path); s(w.query_string); s(w.body); s(w.remote_addr); s(w.content_type);
                s(w.host); s(w.csrf_base); s(w.csrf_exempt_rule);
                b.push_back(w.is_https ? 1 : 0); b.push_back(w.route_matched ? 1 : 0);
                m(w.get_params); m(w.post_params); m(w.cookies_in); m(w.headers_in); m(w.route_params);
                b.shrink_to_fit();
                return b;
            };
            auto unpack_req = [](const std::string& b, look::WebContext& w) {
                size_t i = 0;
                auto u = [&b, &i]() { uint32_t n; std::memcpy(&n, b.data() + i, 4); i += 4; std::string v = b.substr(i, n); i += n; return v; };
                auto m = [&b, &i, &u](std::map<std::string, std::string>& mp) {
                    uint32_t n; std::memcpy(&n, b.data() + i, 4); i += 4;
                    for (uint32_t k = 0; k < n; ++k) { std::string key = u(); mp.emplace_hint(mp.end(), std::move(key), u()); }
                };
                w.method = u(); w.path = u(); w.query_string = u(); w.body = u(); w.remote_addr = u(); w.content_type = u();
                w.host = u(); w.csrf_base = u(); w.csrf_exempt_rule = u();
                w.is_https = b[i++] != 0; w.route_matched = b[i++] != 0;
                m(w.get_params); m(w.post_params); m(w.cookies_in); m(w.headers_in); m(w.route_params);
            };
            // Origin of the callback: the handshake request (packed; empty for SSE) + the generation it was registered in.
            // The generation is held: the callback is that program's bytecode and always runs with its values;
            // the old generation is freed when its last connection closes (same rule as a running HTTP request).
            struct CbOrigin { std::string req; std::shared_ptr<const HttpApp::Snap> gen; };
            auto run_cb = [unpack_req](const look::Value& fnv, std::vector<look::Value> cb_args, const char* tag, const char* kind,
                             const CbOrigin* origin) {
                look::WebContext ctx;
                if (!origin->req.empty()) unpack_req(origin->req, ctx);
                else ctx.method = "__EVENT__";
                direct().gen = origin->gen;
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
                direct().gen.reset();
            };
            // ws::on is called inside the handshake request: copy the context NOW (while the request is alive).
            // The callback runs on another thread after the request is over; each event gets its own fresh copy.
            req_builtins[BI("ws::on")] = [ws_of, run_cb, pack_req, snap](std::vector<look::Value>& args) -> look::Value {
                const look::WebContext* web_now = look::current_request_web();
                if (!web_now) throw std::runtime_error("ws::on() runs only inside a request");
                if (args.size() != 3) throw std::runtime_error("ws::on() takes 3 arguments");
                auto conn = ws_of(args[0], "ws::on() first argument must be a websocket");
                if (args[2].type() != look::Value::BYTECODE_FN)
                    throw std::runtime_error("ws::on() third argument must be a function");
                const std::string ev = args[1].to_string();
                look::Value fnv = args[2].clone_for_thread();
                auto handshake = std::make_shared<const CbOrigin>(CbOrigin{pack_req(*web_now), snap});
                if (ev == "message")
                    conn->set_on_message([fnv, run_cb, handshake](const std::string& msg, bool binary) {
                        // fn($data) or fn($data, $binary): the call ignores the extra argument (old handlers are the same)
                        run_cb(fnv, { look::Value(msg), look::Value(binary) }, "ws", "ws message callback", handshake.get()); });
                else if (ev == "close")
                    conn->set_on_close([fnv, run_cb, handshake]() { run_cb(fnv, {}, "ws", "ws close callback", handshake.get()); });
                return look::Value();
            };
            req_builtins[BI("sse::on")] = [sse_of, run_cb, snap](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 3) throw std::runtime_error("sse::on() takes 3 arguments");
                auto conn = sse_of(args[0], "sse::on() first argument must be an SSE connection");
                if (args[2].type() != look::Value::BYTECODE_FN)
                    throw std::runtime_error("sse::on() third argument must be a function");
                look::Value fnv = args[2].clone_for_thread();
                auto origin = std::make_shared<const CbOrigin>(CbOrigin{std::string(), snap});
                if (args[1].to_string() == "close")
                    conn->set_on_close_cb([fnv, run_cb, origin]() { run_cb(fnv, {}, "sse", "sse close callback", origin.get()); });
                return look::Value();
            };
            // A text frame must be valid UTF-8 (RFC 6455 §8.1). OLD: binary data went silently with a text frame
            // and the browser closed the connection (the application could not see the reason).
            req_builtins[BI("ws::send")] = [ws_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 2) throw std::runtime_error("ws::send() takes 2 arguments");
                auto conn = ws_of(args[0], "ws::send() first argument must be a websocket");
                const std::string msg = args[1].to_string();
                if (!look::ws_utf8_valid(msg))
                    throw std::runtime_error("ws::send(): the message is not valid UTF-8 text; send binary data with ws::send_binary()");
                return look::Value(conn->send_text(msg));
            };
            req_builtins[BI("ws::send_binary")] = [ws_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 2) throw std::runtime_error("ws::send_binary() takes 2 arguments");
                return look::Value(ws_of(args[0], "ws::send_binary() first argument must be a websocket")->send_binary(args[1].to_string()));
            };
            req_builtins[BI("ws::close")] = [ws_of](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 1) throw std::runtime_error("ws::close() takes 1 argument");
                ws_of(args[0], "ws::close() argument must be a websocket")->close_conn();
                return look::Value();
            };
            req_builtins[BI("ws::broadcast")] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 1) throw std::runtime_error("ws::broadcast() takes 1 argument");
                const std::string msg = args[0].to_string();
                if (!look::ws_utf8_valid(msg))
                    throw std::runtime_error("ws::broadcast(): the message is not valid UTF-8 text; send binary data with ws::broadcast_binary()");
                look::g_ws_registry.broadcast(msg);
                return look::Value();
            };
            req_builtins[BI("ws::broadcast_binary")] = [](std::vector<look::Value>& args) -> look::Value {
                if (args.size() != 1) throw std::runtime_error("ws::broadcast_binary() takes 1 argument");
                look::g_ws_registry.broadcast_binary(args[0].to_string());
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
            return look::Value(look::make_channel(args));   // value.h: same rule as the command line
        };
        // args() — no CLI arguments on web/CGI -> empty array (meaningful only for the CLI).
        req_builtins[BI("args")] = [](std::vector<look::Value>&) -> look::Value {
            return look::Value(std::make_shared<look::ValueArray>());
        };
        // env() and config()
        req_builtins[BI("env")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty()) return look::Value();
            std::string key = args[0].to_string();
            std::string def = (args.size() >= 2) ? args[1].to_string() : "";
            // lk-cgi: request headers are in the process environment — on an exempt request the cookie is not read from here either
            if (key == "HTTP_COOKIE") look::csrf_exempt_cookie_guard(look::current_request_web(), "env(\"HTTP_COOKIE\")");
            return look::Value(look::look_get_env(key, def));
        };
        {
            auto f_cfg = copy->get_module_fn("", "config");
            if (f_cfg) req_builtins[BI("config")] = [f_cfg](std::vector<look::Value>& args) -> look::Value {
                std::vector<look::Value> a = args; return f_cfg(a);
            };
        }

        // before_route() — a no-op at dispatch (registered at setup)
        req_builtins[BI("before_route")] = [](std::vector<look::Value>&) -> look::Value { return look::Value(); };

        // stop() — cancel route execution from inside before_route middleware
        req_builtins[BI("stop")] = [](std::vector<look::Value>&) -> look::Value {
            throw look::RouteStopException();
        };

        // exit()/die() — same semantics as the command line (ExitException). If it is not bound
        // the builtin slot stays empty -> std::bad_function_call (previously the route was PERMANENTLY
        // dropped to the interpreter). exit() is rare on the web anyway.
        {
            auto do_exit = [](std::vector<look::Value>& a) -> look::Value {
                int code = 0;
                if (!a.empty() && a[0].type() == look::Value::INT) code = (int)a[0].as_int();
                throw look::ExitException(code);
            };
            req_builtins[BI("exit")] = do_exit;
            req_builtins[BI("die")]  = do_exit;
        }

        // scalar core builtins — single definition (bind_scalar_builtins); the setup table uses it too
        bind_scalar_builtins(req_builtins);
        // push/pop: they were in builtin_names but were NOT bound in req_builtins ->
        // a bare push/pop inside a route threw "bad function call" and dropped
        // to the interpreter. In-place mutation: args[0] shares a shared_ptr with
        // the caller's $arr (the same vector even if copied) -> $arr grows/shrinks in place.
        req_builtins[BI("push")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.size() < 2 || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("push() requires array and value");
            look::value_push_not_map(args[0], "push()");
            args[0].detach();   // do not modify the shared store in place (the variable path comes via PUSH_PATH)
            args[0].as_array()->push_back(args[1]);
            return args[0];
        };
        req_builtins[BI("pop")] = [](std::vector<look::Value>& args) -> look::Value {
            if (args.empty() || args[0].type() != look::Value::ARRAY)
                throw std::runtime_error("pop() requires array");
            look::value_list_only(args[0], "pop()");
            args[0].detach();
            auto a = args[0].as_array();
            if (a->empty()) return look::Value();
            look::Value last = a->back();
            a->pop_back();
            return last;
        };

        if (!local_table) tl_builtins_of = snap;   // valid: until the next hot reload
        }   // end if(need_build) — once per generation per thread (nested dispatch: its own table)

        // before_route closures: same structure as route_closures
        std::vector<look::Closure*> before_closures;
        before_closures.reserve(snap->vm_before_routes.size());
        for (auto& val : snap->vm_before_routes)
            if (val.type() == look::Value::BYTECODE_FN)
                before_closures.push_back(val.as_bytecode_fn().get());

        t_dispatch_start = std::chrono::steady_clock::now();
        bool vm_ok = false;
        bool stopped_direct = false;   // timer callback was called directly
        std::string err_place;         // the line of the error — appended to the log line
        try {
            // Memory (look/mem_budget.h): if 90% of the process limit is used a new request gets 503 (those in
            // progress continue); the request account is set up before the VM and torn down after. Nested dispatch (from inside a route
            // jobs::run) uses the outer request's account.
            if (!nested && !look::mem_admit())
                throw look::ServiceUnavailable("request refused: the server is near its memory limit (" +
                    look::mem_format_bytes(look::mem_process_bytes()) + " of " +
                    look::mem_format_bytes(look::mem_process_limit()) + ", LOOK_MEMORY_LIMIT)");
            std::optional<look::MemScope> mem_scope;
            if (!nested) mem_scope.emplace();
            look::VM::SharedState sh;
            sh.routes         = &route_closures;
            sh.builtins       = &req_builtins;
            look::VM vm(sh, output);
            VmErrorPlace vm_place{vm, err_place};
            // The setup globals are shared with the request (the map is copied per request, the values
            // are shared); if the request writes to one, the write path takes its own copy first.
            vm.set_globals(snap->vm_setup_globals);
            vm.set_web_context(&web);
            // The connection handler (WS/SSE route) is in the VM — the connection goes to the handler as the first argument.
            if (direct().conn.type() == look::Value::WEBSOCKET) vm.set_ws_connection(direct().conn.as_websocket());
            else if (direct().conn.type() == look::Value::SSE_CONN) vm.set_sse_connection(direct().conn.as_sse());

            // Timer callback: no route matching and no before_route; the closure
            // is called directly in this VM set up like a new request (clean setup values, its own builtins).
            // The error falls to the common catcher below (log + no re-run).
            if (direct().closure) {
                direct().result = vm.call_closure(*direct().closure,
                                direct().args ? *direct().args : std::vector<look::Value>{});
                stopped_direct = true;
            }
            // run the before_route middlewares in order
            // when stop() throws, RouteStopException is caught -> the route is skipped
            bool stopped = stopped_direct;
            // Default CSRF gate — BEFORE the route and before_route, the same place in all three modes. The connection
            // handler (WS/SSE) and the timer callback are not requests.
            // A body that cannot be parsed (invalid multipart boundary, part limit exceeded): an explicit response
            // before the route — previously the fields were silently empty and the application thought "missing field".
            if (!stopped && web.body_error_status != 0 && direct().conn.type() == look::Value::NONE) {
                stopped = true;
                web.status_code = web.body_error_status;
                web.status_text = web.body_error_status == 413 ? "Payload Too Large" : "Bad Request";
                web.headers_out["Content-Type"] = "text/plain; charset=utf-8";
                web.response_body = web.status_text + ": " + web.body_error;
                look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                    web.method + " " + web.path + " refused: " + web.body_error);
            }
            // Every method OTHER THAN the safe ones (GET/HEAD/OPTIONS) (Go 1.25 CrossOriginProtection).
            // The list used to be POST/PUT/PATCH/DELETE: routes like route("PROPFIND"/"PURGE", ...) never
            // went through the gate (measured) — a new method silently stayed unprotected.
            if (!stopped && snap->csrf_on && direct().conn.type() == look::Value::NONE &&
                web.method != "GET" && web.method != "HEAD" && web.method != "OPTIONS") {
                for (const auto& rule : snap->csrf_exempt)
                    if (csrf_exempt_match(rule, web.path)) { web.csrf_exempt_rule = rule; break; }
                std::string why;
                if (web.csrf_exempt_rule.empty() && !look::csrf_request_ok(&web, why)) {
                    stopped = true;
                    web.status_code = 403;
                    web.status_text = "Forbidden";
                    web.headers_out["Content-Type"] = "text/plain; charset=utf-8";
                    web.response_body = "Forbidden: this form has expired or did not come from this site. "
                                        "Reload the page and try again.";
                    look::Logger::instance().log(look::LogLevel::LOG_WARN, "CSRF",
                        web.method + " " + web.path + " refused: " + why + ". Every application that uses cookies is "
                        "protected: put csrf::field() in the form, or send csrf::token() in an X-CSRF-Token header; "
                        "a webhook or token API that does not use cookies can be exempted with csrf::exempt(\"" +
                        web.path + "\")");
                }
            }
            if (!stopped) for (auto* cl : before_closures) {
                try {
                    vm.call_closure(*cl, {});
                } catch (const look::RouteStopException&) {
                    stopped = true;
                    break;
                }
            }

            if (!stopped) {
                if (snap->script_mode) {
                    // A script without routes (like PHP): the whole program runs in this request, with fresh globals.
                    look::VM script_vm(sh, output);
                    VmErrorPlace script_place{script_vm, err_place};
                    script_vm.set_web_context(&web);
                    script_vm.execute(*snap->compiled);
                } else {
                    vm.dispatch_routes(web.method, web.path);
                }
            }
            vm_ok = true;
        } catch (const look::RouteStopException&) {
            vm_ok = true;  // stop() was called inside the route handler — normal flow
        } catch (const look::ExitException&) {
            // exit()/die() — terminates the script, NOT a VM error. If it fell into the generic catch
            // it would be 500 + an error log (a false accusation).
            // The output written up to that point becomes the response.
            vm_ok = true;
        } catch (const std::exception& vm_e) {
            // Runtime error: 500 + log. There is no interpreter — no route pinning either.
            //
            // History — C9: previously on the first error the route was permanently pinned to the interpreter.
            // This safety net MASKED BUGS for years — since the route silently dropped to the slow path and returned the CORRECT result
            // nobody noticed (most of the 10 bugs found in the 2026-07-16 round were hidden
            // this way); the log level had been lowered to ERROR, LOOK_VM_STRICT=1 closed the net.
            //
            // 1.0.8 — THE HANDLER IS NEVER RE-RUN. The "safety net" above counted every error
            // (a plain `throw`, a database error, an undefined variable) as a "VM error" and ran the SAME request
            // again from the start in the interpreter: every side effect done before the error (INSERT,
            // jobs::push, an outside HTTP request) happened TWICE in one request, and the route was permanently
            // pinned to the slow path (triggerable by user input). Measured:
            // a route doing "INSERT; then error" wrote 2 rows in a single request.
            // LOOK_VM_STRICT=1 used to rethrow here: the exception escaped the worker,
            // connections were not released and after as many errors as workers the server hung.
            const std::string what = vm_e.what();
            const auto* mem_e = dynamic_cast<const look::MemoryLimitError*>(&vm_e);
            if (dynamic_cast<const look::ServiceUnavailable*>(&vm_e) || (mem_e && mem_e->server_full)) {
                // A request stopped because the server is full (look/mem_budget.h) is also 503: it is not its own error.
                // A dependent service outage (e.g. the Redis session store): not an application error ->
                // 503 and a short explanation instead of a half-finished page. The error is still written to the log.
                web.status_code = 503;
                web.status_text = "Service Unavailable";
                output.str("");
                output.clear();
                web.response_body = "Service temporarily unavailable. Please try again shortly.";
                web.headers_out["Content-Type"] = "text/plain; charset=utf-8";
                web.headers_out["Retry-After"] = "1";   // temporary: the client should retry shortly
            } else if (web.status_code < 400) {
                // Error = 500. Previously only if it was 200: if the route had said response::status(201) / 302
                // before the error, the failed request went out as "201 Created" — the client
                // thought the failed operation succeeded. A 4xx/5xx the application set itself is preserved.
                web.status_code = 500;
                web.status_text = "Internal Server Error";
            }
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
                (direct().what ? std::string(direct().what) + " error: " : std::string("Dispatch error: ")) + what + err_place);
            if (direct().closure) direct().failed = true;
            vm_ok = true;   // response ready (500) — NO RE-RUN in the interpreter
        }

        // Connections acquired lazily during the request are returned here — covers both the successful and
        // the failed flow. A missing release made all workers deadlock
        // after pool_size requests.
        if (!nested) look::release_thread_connections();
    } else {
        // The application could not be loaded (hot reload failed: the VM could not compile/set up).
        // No interpreter fallback — the request gets an explicit 500; the next load recovers once the file is fixed.
        t_dispatch_start = std::chrono::steady_clock::now();
        web.status_code = 500;
        web.status_text = "Internal Server Error";
        output << "The application could not be loaded; see the server log.";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
            "Request refused: the application is not loaded (the last reload failed; see the error above).");
    }

    auto t_dispatch_end = std::chrono::steady_clock::now();
    auto us_dispatch = std::chrono::duration_cast<std::chrono::microseconds>(t_dispatch_end - t_dispatch_start).count();

    // Process-global counters (for runtime::stats, read-only observability). RuntimeHost does not
    // count requests -> these globals carry the real HTTP state. The clocks
    // were already called -> latency accumulation ADDS no new clock to the hot path (only a relaxed atomic).
    // timer/connection callbacks and the connection handler are not HTTP requests: they do not enter the counters
    if (direct().closure || direct().conn.type() != look::Value::NONE) return;
    look::g_http_request_count().fetch_add(1, std::memory_order_relaxed);
    look::g_latency_us_sum().fetch_add((uint64_t)(us_dispatch < 0 ? 0 : us_dispatch), std::memory_order_relaxed);
    look::g_latency_us_last().store((uint64_t)(us_dispatch < 0 ? 0 : us_dispatch), std::memory_order_relaxed);
    if (web.status_code >= 500)
        look::g_error_5xx_count().fetch_add(1, std::memory_order_relaxed);

    // log timing for the first 200 requests — for profiling
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
    // If the client closes the connection early write() sends SIGPIPE — the default behavior terminates the process
    signal(SIGPIPE, SIG_IGN);
    // fill the tz global state once before the worker threads start —
    // makes the tzset access inside localtime_r race-free (TSan date race fix).
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
        // The upper bound of the pool is independent of the worker count: LOOK_DB_POOL_SIZE if given, otherwise
        // the default (db::connect: 4 x CPU, 8..64). Connections are opened as needed.
        const char* env = std::getenv("LOOK_DB_POOL_SIZE");
        int pool_sz = (env && env[0]) ? std::max(1, std::min(std::atoi(env), 256)) : 0;
        look::set_db_pool_size(pool_sz);

        // LOOK_DB_ASYNC_THREADS was removed (AsyncDbPool was never used; it only
        // spawned idle threads). A warning so that whoever left it set sees it — it must not be silently ignored.
        const char* ath_env = std::getenv("LOOK_DB_ASYNC_THREADS");
        if (ath_env && ath_env[0])
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "HTTP",
                "LOOK_DB_ASYNC_THREADS is no longer used and has no effect; remove it. "
                "Database connections are set with LOOK_DB_POOL_SIZE.");
    }

    // Initial setup
    try {
        std::unique_lock<std::shared_mutex> ul(g_http_mutex);
        run_setup_http(script);
    } catch (const std::exception& e) {
        std::cerr << "[look-fcgi --mode http] Setup error: " << e.what() << "\n";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP",
            std::string("Setup error: ") + e.what() + " — the application does not start.");
        std::exit(1);   // an application that does not start must not exit with a success code (it used to return 0)
    }

    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP",
        "look-fcgi --mode http port=" + std::to_string(port) + " workers=" + std::to_string(workers));
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Script: " + script.string());
    look::Logger::instance().log(look::LogLevel::LOG_INFO, "HTTP", "Apache bypass — use nginx or a direct connection.");
    // Settings that weaken security: one WARN line each (look/weak_settings.h).
    for (const auto& w : look::weak_setting_warnings(std::getenv, false))
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "SECURITY", w);

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
        web.remote_addr = resolve_client_ip(req);   // WS/SSE: same contract (see resolve_client_ip)
        web.is_https    = resolve_is_https(req);
        web.host        = resolve_host(req);
        // Headers and cookies as in an ordinary request: request::header / session:: / cookie:: work in the WS
        // route and in callbacks (previously empty -> WS authentication with a cookie
        // was impossible, silently null).
        web.headers_in = req.headers;
        if (auto ck = req.headers.find("cookie"); ck != req.headers.end())
            web.cookies_in = look::WebContext::parse_cookies(ck->second);
        // CSWSH (another site opening a WS with the visitor's cookie) is rejected BEFORE the handshake in http_server.cpp
        // (handle_ws_upgrade): same origin by default, the LOOK_WS_ORIGINS allow list.

        // The connection handler is dispatched in the VM like an ordinary request.
        // The connection is carried with direct().conn and given to the handler as the first argument.
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
        web.remote_addr = resolve_client_ip(req);   // WS/SSE: same contract (see resolve_client_ip)
        web.is_https    = resolve_is_https(req);
        web.host        = resolve_host(req);
        // Same as WS: headers and cookies. (No Origin check needed: EventSource is a GET and
        // another site reading the stream is blocked by CORS.)
        web.headers_in = req.headers;
        if (auto ck = req.headers.find("cookie"); ck != req.headers.end())
            web.cookies_in = look::WebContext::parse_cookies(ck->second);

        // The connection handler is dispatched in the VM like an ordinary request.
        // The connection is carried with direct().conn and given to the handler as the first argument.
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

#ifndef _WIN32   // The embedded mail servers are POSIX-only (OpenSSL). Windows = XAMPP dev.
    // SMTP server — starts together with HTTP if the LOOK_SMTP_PORT env var is set
    // Port environment variable: digits only, 1-65535 (look/host_port.h). Previously atoi: "25x" -> 25,
    // "abc" -> 0 -> the server silently did not start without leaving any trace. A bad value -> an explicit ERROR, that server does not start.
    // An empty/undefined optional variable -> 0 (none).
    auto env_port = [](const char* name, int& out) -> bool {
        out = 0;
        const char* e = std::getenv(name);
        if (!e || !*e) return true;
        if (look::parse_port_number(e, out)) return true;
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "config",
            std::string(name) + "=\"" + e + "\" is not a valid port (expected a number from 1 to 65535)");
        return false;
    };
    std::unique_ptr<look::SmtpServer> smtp_srv;
    std::thread smtp_thread;
    const char* smtp_port_env = std::getenv("LOOK_SMTP_PORT");
    if (smtp_port_env && *smtp_port_env) {
        int smtp_port = 0, sub_port = 0;
        bool ports_ok = env_port("LOOK_SMTP_PORT", smtp_port) && env_port("LOOK_SMTP_SUB_PORT", sub_port);
        if (!ports_ok)
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "smtp", "SMTP server not started (invalid port setting)");
        if (ports_ok && smtp_port > 0) {
            smtp_srv = std::make_unique<look::SmtpServer>(
                smtp_port, sub_port, 0, workers,
                [](const look::SmtpMessage& msg) -> bool {
                    // Default handler: write to every local recipient's Maildir.
                    // Path: <base>/<recipient>/inbox — the same place IMAP LOGIN reads
                    // (aligned with mail_user_auth). Recipient name email = LOGIN user name.
                    const char* base = std::getenv("LOOK_MAIL_DIR");
                    if (!base || !*base) base = "/var/mail/look";
                    bool any = false;
                    for (const std::string& rcpt : msg.rcpt_to) {
                        // SECURITY: handle traversal without making the recipient a directory component
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
            // The "started" log must depend on the REAL state. Previously it was unconditional:
            // when bind failed the log looked like this (measured) —
            //   [ERROR] port 7171 could not be listened on
            //   [INFO ] IMAP server started on port 7171   <- misleading
            // The user looked at the log and thought it was running; a silent failure.
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

    // IMAP server — starts together with HTTP+SMTP if LOOK_IMAP_PORT is set.
    // Identity: mail_user_auth (same model as SMTP AUTH). INBOX = what SMTP wrote to
    // <LOOK_MAIL_DIR>/<user>/inbox. LOOK_IMAP_CERT/KEY for STARTTLS/IMAPS.
    std::unique_ptr<look::ImapServer> imap_srv;
    const char* imap_port_env = std::getenv("LOOK_IMAP_PORT");
    if (imap_port_env && *imap_port_env) {
        int imap_port = 0, imaps_port = 0;
        bool ports_ok = env_port("LOOK_IMAP_PORT", imap_port) && env_port("LOOK_IMAP_PORT_TLS", imaps_port);
        if (!ports_ok)
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "imap", "IMAP server not started (invalid port setting)");
        if (ports_ok && imap_port > 0) {
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
            // start() now returns bool — the "started" log depends on the real state
            // (see the same fix in SMTP: "started" was written after a bind error).
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
#endif  // _WIN32 — mail servers

    try {
        look::HttpServer server(port, workers, http_handler, ws_handler, sse_handler);
        // Graceful shutdown (look/shutdown.h): SIGTERM/SIGINT stop new connections, those in progress finish,
        // run() returns when the workers finish.
        look::shutdown::on_begin([&server] { server.begin_shutdown(); });
        server.run();   // "Listening on ..." is written by run() — when the port is really being listened on (workers ready)
        if (look::shutdown::requested()) {
#ifndef _WIN32
            if (imap_srv) { imap_srv->stop(); }
            if (smtp_srv) { smtp_srv->stop(); }
            if (smtp_thread.joinable()) smtp_thread.join();
#endif
            look::shutdown::finish(0);
        }
    } catch (const std::exception& e) {
        // The server could not be set up/run: EXIT with an error code. Previously it logged and returned normally ->
        // the process ended with 0, systemd Restart=on-failure counted this as success and did not restart.
        std::cerr << "[look-fcgi --mode http] " << e.what() << "\n";
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "HTTP", e.what());
        std::fflush(nullptr);
        std::_Exit(1);
    }

#ifndef _WIN32
    if (imap_srv) { imap_srv->stop(); }
    if (smtp_srv) { smtp_srv->stop(); }
    if (smtp_thread.joinable()) smtp_thread.join();
#endif
}

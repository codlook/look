#include "look/runtime_host.h"
#include "look/stdlib.h"
#include "look/web.h"
#include "look/logger.h"
#include "look/jobs_store.h"
#include "look/setup_handoff.h" // vm_closure_runner — jobs::run işleyicisi
#include <thread>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <climits>
#include <cstdint>
#include <stdexcept>

namespace look {

// ── RuntimeHost (modül ev sahibi) ─────────────────────────────────────────────

static void init_core_modules(std::map<std::string, Module>& stdlib,
                               std::map<std::string, Module>& modules) {
    // log:: is a core module — accessible via :: without 'use log;'
    if (stdlib.count("log"))      modules["log"]      = stdlib["log"];
    if (stdlib.count("file"))     modules["file"]     = stdlib["file"];
    if (stdlib.count("date"))     modules["date"]     = stdlib["date"];
    if (stdlib.count("error"))    modules["error"]    = stdlib["error"];
    if (stdlib.count("parallel")) modules["parallel"] = stdlib["parallel"];
    if (stdlib.count("auth"))     modules["auth"]     = stdlib["auth"];
}

RuntimeHost::RuntimeHost() {
    output_stream_ = &std::cout;
    stdlib_ = make_stdlib(this);
    auto extra = make_extra_stdlib(this);
    for (auto& [k,v] : extra) stdlib_[k] = std::move(v);
    init_core_modules(stdlib_, modules_);
    register_app_module();
}

RuntimeHost::RuntimeHost(std::ostream& out) {
    output_stream_ = &out;
    stdlib_ = make_stdlib(this);
    auto extra = make_extra_stdlib(this);
    for (auto& [k,v] : extra) stdlib_[k] = std::move(v);
    init_core_modules(stdlib_, modules_);
    register_app_module();
}

// ── app:: — servis kaydı (use gerekmez, core modül) ───────────────────────────
// app::set("db", $conn)  → setup'ta paylaşılan servis kaydeder
// app::get("db") / app::db()  → route içinde capture (use) olmadan erişim
// Lambda'lar `this`'i capture eder; çağrı anında this->services_ (paylaşılan
// registry) okunur — clone sonrası da doğru registry'ye bakar.
void RuntimeHost::register_app_module() {
    RuntimeHost* self = this;
    Module app;
    app.name = "app";
    app.functions["set"] = [self](std::vector<Value> args) -> Value {
        if (args.size() < 2) return Value();
        std::lock_guard<std::mutex> lk(self->services_->mtx);
        self->services_->items[args[0].to_string()] = args[1];
        return args[1];
    };
    app.functions["get"] = [self](std::vector<Value> args) -> Value {
        if (args.empty()) return Value();
        std::lock_guard<std::mutex> lk(self->services_->mtx);
        auto it = self->services_->items.find(args[0].to_string());
        return it == self->services_->items.end() ? Value() : it->second;
    };
    app.functions["has"] = [self](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(false);
        std::lock_guard<std::mutex> lk(self->services_->mtx);
        return Value(self->services_->items.count(args[0].to_string()) > 0);
    };
    // app::db() — en sık kullanılan servis için kısayol (== app::get("db"))
    app.functions["db"] = [self](std::vector<Value>) -> Value {
        std::lock_guard<std::mutex> lk(self->services_->mtx);
        auto it = self->services_->items.find("db");
        return it == self->services_->items.end() ? Value() : it->second;
    };
    stdlib_["app"]  = app;   // setup'ta erişilebilir
    modules_["app"] = app;   // dispatch (VM get_module_fn) erişebilir
}

// ── İş parçacığı kopyası ──────────────────────────────────────────────────────
// Her web işçisi (ve iş/zamanlayıcı işleyicisi) kendi çıktısı ve istek bağlamıyla bir kopya
// kullanır. Kurulumda `use` edilen modüller ve app:: servis kaydı paylaşılır (servisler
// sözleşme gereği değişken durum tutmaz; kayıt mutex'li).
std::unique_ptr<RuntimeHost> RuntimeHost::make_dispatch_copy() const {
    auto c = std::make_unique<RuntimeHost>();   // stdlib_ taze kurulur
    c->modules_      = modules_;                // kurulumdaki `use` durumu
    c->services_     = services_;               // PAYLAŞ (app::)
    c->main_script_  = main_script_;
    c->current_file_ = current_file_;
    c->start_time_   = start_time_;
    c->route_count_  = route_count_;
    return c;
}

// ── .env loader ───────────────────────────────────────────────────────────────

static std::map<std::string, std::string> g_env_vars;
static bool g_env_loaded = false;

static void load_env_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim whitespace
        while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
        while (!val.empty() && isspace((unsigned char)val.front())) val = val.substr(1);
        // Remove surrounding quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        g_env_vars[key] = val;
    }
    g_env_loaded = true;
}

static void ensure_env_loaded() {
    if (g_env_loaded) return;
    load_env_file(".env");
    if (!g_env_loaded) load_env_file("../.env");
    g_env_loaded = true;

    // APP_ENV'e gore log seviyesi ayarla
    auto env_it = g_env_vars.find("APP_ENV");
    std::string app_env = (env_it != g_env_vars.end()) ? env_it->second : "production";

    auto debug_it = g_env_vars.find("APP_DEBUG");
    bool debug = (debug_it != g_env_vars.end() && debug_it->second == "true");

    auto log_dir_it = g_env_vars.find("LOG_DIR");
    std::string log_dir = (log_dir_it != g_env_vars.end()) ? log_dir_it->second : "logs";
    // Path traversal koruması: ../ ve mutlak path reddedilir
    if (log_dir.find("..") != std::string::npos ||
        (!log_dir.empty() && (log_dir[0] == '/' || log_dir[0] == '\\')))
        log_dir = "logs";

    bool verbose    = (app_env == "development") || debug;
    LogLevel level  = (app_env == "development" || debug) ? LogLevel::LOG_DEBUG : LogLevel::LOG_INFO;

    Logger::instance().configure(log_dir, verbose, level);
}

void RuntimeHost::set_web_context(WebContext* ctx) {
    web_ctx_ = ctx;
    // Web core modülleri: stdlib_'e VE modules_'a ekle
    // use gerekmez — db::, request::, response:: vb. doğrudan erişilebilir
    if (ctx) {
        auto web_mods = make_web_modules(ctx, this);
        for (auto& [name, mod] : web_mods) {
            stdlib_[name]  = mod;
            modules_[name] = std::move(mod);
        }
    }
}

// Geri çağırma çıktısı: komut satırında stdout, web'de ayrı tampon (bkz. interpreter.h).
std::ostream& RuntimeHost::callback_output(const std::shared_ptr<std::ostringstream>& sink) const {
    return output_stream_ == &std::cout ? std::cout : static_cast<std::ostream&>(*sink);
}

bool RuntimeHost::load_stdlib_module(const std::string& name) {
    auto it = stdlib_.find(name);
    if (it == stdlib_.end()) return false;   // dış/paket modül — CLI-VM kapsam dışı
    modules_[name] = it->second;
    return true;
}

NativeFn RuntimeHost::get_module_fn(const std::string& module_name, const std::string& fn_name) const {
    auto mit = modules_.find(module_name);
    if (mit == modules_.end()) return nullptr;
    auto fit = mit->second.functions.find(fn_name);
    if (fit == mit->second.functions.end()) return nullptr;
    return fit->second;
}

// ── VM/interpreter callback köprüsü (bridge tarafı) ───────────────────────────
// interpreter.cpp hem CLI hem fcgi'ye linklenir. VM (vm.cpp) run() içinde hook'unu
// register_vm_bridge ile kaydeder; CLI'da hiç kaydolmaz → available()==false.
namespace { thread_local Value (*g_vm_bridge)(const Value&, std::vector<Value>&) = nullptr; }
void  register_vm_bridge(Value (*hook)(const Value&, std::vector<Value>&)) { g_vm_bridge = hook; }
bool  vm_bridge_available() { return g_vm_bridge != nullptr; }
Value vm_bridge_invoke(const Value& fn, std::vector<Value>& args) {
    if (!g_vm_bridge) throw std::runtime_error("vm_bridge_invoke: no VM hook registered");
    return g_vm_bridge(fn, args);
}

// ── jobs::run ────────────────────────────────────────────────────────────────
// Kayıtlı kuyruk işleyicilerini (jobs::worker) çalıştırır. interval_ms == 0 → tek tur, döner;
// > 0 → sonsuz döngü (worker süreci). Web ve komut satırı bu TEK tanımı kullanır. İşleyici
// vm_closure_runner kayıtlıysa yeni bir istek gibi kendi VM'inde, değilse invoke() köprüsüyle
// çağıranın VM'inde çalışır.
Value RuntimeHost::jobs_run(int interval_ms) {
    bool once = (interval_ms == 0);
    auto& workers = look::JobStore::instance().workers();
    if (workers.empty())
        throw std::runtime_error("jobs::run() — register a handler first with jobs::worker()");

    auto run_one_pass = [&]() {
        for (auto& [queue, fn_v] : workers) {
            while (true) {
                Value job = look::JobStore::instance().next(queue);
                if (job.type() == Value::NONE) break;

                int64_t job_id = 0;
                if (job.as_array()) {
                    auto& arr = *job.as_array();
                    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                        if (arr[i].type() == Value::STRING && arr[i].as_string() == "id") {
                            job_id = (int64_t)arr[i + 1].to_float();
                            break;
                        }
                    }
                }

                auto copy = make_dispatch_copy();
                auto sink = std::make_shared<std::ostringstream>();
                copy->set_output(callback_output(sink));   // komut satırında stdout (eskiden atılıyordu)
                // İşleyici KENDİ istek bağlamında koşar. Eskiden kopya, jobs::run'ı çağıran isteğin
                // bağlamını paylaşıyordu: işleyici o isteğin parametrelerini okuyor, response::status /
                // header çağrıları isteğin yanıtını değiştiriyordu.
                look::WebContext job_ctx;
                job_ctx.method = "__JOB__";
                copy->set_web_context(&job_ctx);
                // LOOK 2: işleyici true (bitti) ya da false (başarısız, beklemeyle yeniden denenir)
                // döndürür. Başka bir şey döndürmek — en çok da `return`i unutmak — HATADIR: eskiden
                // sessizce "başarısız" sayılıyor ve iş yeniden çalıştırılıyordu (e-posta üç kez gidiyordu).
                // Artık loglanır ve iş YENİDEN ÇALIŞTIRILMADAN başarısız işaretlenir.
                enum class Outcome { Done, Retry, BadReturn } outcome = Outcome::Retry;
                std::string returned;
                try {
                    Value result;
                    bool ran = true;
                    if (fn_v.type() == Value::BYTECODE_FN && look::vm_closure_runner()) {
                        // LOOK 2: işleyici VM closure'ı — yeni bir istek gibi VM'de koşar (hata orada loglanır).
                        result = look::vm_closure_runner()(fn_v, {job}, "job handler", ran);
                    } else {
                        result = copy->invoke(fn_v, {job});
                    }
                    if (!ran) outcome = Outcome::Retry;                       // işleyici hata verdi (loglandı)
                    else if (result.type() == Value::BOOL) outcome = result.as_bool() ? Outcome::Done : Outcome::Retry;
                    else {
                        outcome = Outcome::BadReturn;
                        returned = result.type() == Value::NONE ? "nothing (null)" :
                                   result.type() == Value::STRING ? "a string" :
                                   result.type() == Value::INT || result.type() == Value::FLOAT ? "a number" : "a value that is not true or false";
                    }
                } catch (const std::exception& ex) {
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, "jobs::run",
                        std::string("handler error [") + queue + "]: " + ex.what());
                    outcome = Outcome::Retry;
                }

                if (outcome == Outcome::Done) look::JobStore::instance().done(job_id);
                else if (outcome == Outcome::Retry) look::JobStore::instance().fail(job_id);
                else {
                    look::Logger::instance().log(look::LogLevel::LOG_ERROR, "jobs::run",
                        std::string("handler for queue '") + queue + "' must return true (done) or false (failed, tried again later); it returned "
                        + returned + " — job " + std::to_string(job_id) + " is marked failed and is not run again");
                    look::JobStore::instance().fail_final(job_id);
                }
            }
        }
    };

    if (once) {
        run_one_pass();
        return Value(true);
    }
    while (true) {
        run_one_pass();
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return Value(); // unreachable
}

Value RuntimeHost::invoke(const Value& fn, std::vector<Value> args) {
    // VM closure (BYTECODE_FN): higher-order builtin bir VM route'undan çağrıldı →
    // callback'i aktif VM'e delege et. Eskiden "not a function" fırlatıp route'u
    // interpreter'a düşürüyordu (array::map/filter/reduce bu yüzden fallback ediyordu).
    if (fn.type() == Value::BYTECODE_FN) {
        if (vm_bridge_available()) return vm_bridge_invoke(fn, args);
        throw std::runtime_error("invoke: VM closure — no active VM");
    }
    throw std::runtime_error("invoke: not a function");
}

// ── look_get_env — VM setup için .env-aware env() erişimi ───────────────────
std::string look_get_env(const std::string& key, const std::string& default_val) {
    ensure_env_loaded();
    auto it = g_env_vars.find(key);
    if (it != g_env_vars.end()) return it->second;
    const char* sys = std::getenv(key.c_str());
    if (sys) return std::string(sys);
    return default_val;
}

} // namespace look

#include "look/runtime_host.h"
#include "look/stdlib.h"
#include "look/web.h"
#include "look/logger.h"
#include "look/jobs_store.h"
#include "look/vm_closure_runner.h" // vm_closure_runner — the jobs::run handler
#include <thread>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <climits>
#include <cstdint>
#include <stdexcept>

namespace look {

// ── RuntimeHost (module host) ─────────────────────────────────────────────

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

// ── app:: — service registry (no use needed, core module) ─────────────────────
// app::set("db", $conn)  → registers a shared service at setup
// app::get("db") / app::db()  → access inside a route without capture (use)
// The lambdas capture `this`; at call time this->services_ (the shared
// registry) is read — after clone it also looks at the right registry.
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
    // app::db() — shortcut for the most used service (== app::get("db"))
    app.functions["db"] = [self](std::vector<Value>) -> Value {
        std::lock_guard<std::mutex> lk(self->services_->mtx);
        auto it = self->services_->items.find("db");
        return it == self->services_->items.end() ? Value() : it->second;
    };
    stdlib_["app"]  = app;   // accessible at setup
    modules_["app"] = app;   // dispatch (VM get_module_fn) can access it
}

// ── Thread copy ───────────────────────────────────────────────────────────────
// Each web worker (and job/timer handler) uses a copy with its own output and request context.
// The modules `use`d at setup and the app:: service registry are shared (by contract services
// hold no mutable state; the registry is mutex-protected).
std::unique_ptr<RuntimeHost> RuntimeHost::make_dispatch_copy() const {
    auto c = std::make_unique<RuntimeHost>();   // stdlib_ is built fresh
    c->modules_      = modules_;                // the `use` state from setup
    c->services_     = services_;               // SHARE (app::)
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

    // set the log level according to APP_ENV
    auto env_it = g_env_vars.find("APP_ENV");
    std::string app_env = (env_it != g_env_vars.end()) ? env_it->second : "production";

    auto debug_it = g_env_vars.find("APP_DEBUG");
    bool debug = (debug_it != g_env_vars.end() && debug_it->second == "true");

    auto log_dir_it = g_env_vars.find("LOG_DIR");
    std::string log_dir = (log_dir_it != g_env_vars.end()) ? log_dir_it->second : "logs";
    // Path traversal protection: ../ and absolute paths are rejected
    if (log_dir.find("..") != std::string::npos ||
        (!log_dir.empty() && (log_dir[0] == '/' || log_dir[0] == '\\')))
        log_dir = "logs";

    bool verbose    = (app_env == "development") || debug;
    LogLevel level  = (app_env == "development" || debug) ? LogLevel::LOG_DEBUG : LogLevel::LOG_INFO;

    Logger::instance().configure(log_dir, verbose, level);
}

void RuntimeHost::set_web_context(WebContext* ctx) {
    web_ctx_ = ctx;
    // Web core modules: add to stdlib_ AND modules_
    // no use needed — db::, request::, response:: etc. are directly accessible
    if (ctx) {
        auto web_mods = make_web_modules(ctx, this);
        for (auto& [name, mod] : web_mods) {
            stdlib_[name]  = mod;
            modules_[name] = std::move(mod);
        }
    }
}

void RuntimeHost::bind_web_to_current_request() {
    web_ctx_ = nullptr;
    auto web_mods = make_web_modules(nullptr, this);   // nullptr: the context is read at call time
    for (auto& [name, mod] : web_mods) {
        stdlib_[name]  = mod;
        modules_[name] = std::move(mod);
    }
}

// Callback output: stdout on the command line, a separate buffer on the web (see runtime_host.h).
std::ostream& RuntimeHost::callback_output(const std::shared_ptr<std::ostringstream>& sink) const {
    return output_stream_ == &std::cout ? std::cout : static_cast<std::ostream&>(*sink);
}

bool RuntimeHost::load_stdlib_module(const std::string& name) {
    auto it = stdlib_.find(name);
    if (it == stdlib_.end()) return false;   // external/package module — outside the CLI-VM scope
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

// ── VM callback bridge (bridge side) ────────────────────────────────────────
// The VM (vm.cpp) registers its hook in run() with register_vm_bridge (thread-local);
// if no VM ran on this thread, available()==false.
// fiber-ok: constant hook pointer (the same function on every thread)
namespace { thread_local Value (*g_vm_bridge)(const Value&, std::vector<Value>&) = nullptr; }
void  register_vm_bridge(Value (*hook)(const Value&, std::vector<Value>&)) { g_vm_bridge = hook; }
bool  vm_bridge_available() { return g_vm_bridge != nullptr; }
Value vm_bridge_invoke(const Value& fn, std::vector<Value>& args) {
    if (!g_vm_bridge) throw std::runtime_error("vm_bridge_invoke: no VM hook registered");
    return g_vm_bridge(fn, args);
}

// ── jobs::run ────────────────────────────────────────────────────────────────
// Runs the registered queue handlers (jobs::worker). interval_ms == 0 → one round, returns;
// > 0 → infinite loop (worker process). Web and the command line use this SINGLE definition. If a handler
// vm_closure_runner is registered it runs in its own VM like a new request, otherwise it runs through the invoke() bridge
// in the caller's VM.
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
                copy->set_output(callback_output(sink));   // stdout on the command line (it used to be discarded)
                // The handler runs in ITS OWN request context. Previously the copy shared the context of the
                // request that called jobs::run: the handler read that request's parameters, and response::status /
                // header calls changed that request's response.
                look::WebContext job_ctx;
                job_ctx.method = "__JOB__";
                copy->set_web_context(&job_ctx);
                // LOOK: the handler returns true (done) or false (failed, retried after a wait).
                // Returning anything else — most often forgetting `return` — is an ERROR: it used to
                // be silently counted as "failed" and the job was rerun (the e-mail went out three times).
                // It is now logged and the job is marked failed WITHOUT being rerun.
                enum class Outcome { Done, Retry, BadReturn } outcome = Outcome::Retry;
                std::string returned;
                try {
                    Value result;
                    bool ran = true;
                    if (fn_v.type() == Value::BYTECODE_FN && look::vm_closure_runner()) {
                        // LOOK: handler is a VM closure — runs in the VM like a new request (the error is logged there).
                        result = look::vm_closure_runner()(fn_v, {job}, "job handler", ran);
                    } else {
                        result = copy->invoke(fn_v, {job});
                    }
                    if (!ran) outcome = Outcome::Retry;                       // the handler raised an error (logged)
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
    // VM closure (BYTECODE_FN): a higher-order builtin was called from a VM route →
    // delegate the callback to the active VM. It used to throw "not a function" and drop the route
    // to the interpreter (this is why array::map/filter/reduce fell back).
    if (fn.type() == Value::BYTECODE_FN) {
        if (vm_bridge_available()) return vm_bridge_invoke(fn, args);
        throw std::runtime_error("invoke: VM closure — no active VM");
    }
    throw std::runtime_error("invoke: not a function");
}

// ── look_get_env — .env-aware env() access for VM setup ───────────────────
std::string look_get_env(const std::string& key, const std::string& default_val) {
    ensure_env_loaded();
    auto it = g_env_vars.find(key);
    if (it != g_env_vars.end()) return it->second;
    const char* sys = std::getenv(key.c_str());
    if (sys) return std::string(sys);
    return default_val;
}

} // namespace look

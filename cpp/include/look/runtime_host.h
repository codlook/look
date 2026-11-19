#pragma once
#include "look/value.h"
#include "look/module_host.h"   // ModuleHost — the runtime interface of the built-in modules
#include <sstream>

namespace look {

// ── RuntimeHost — the host of the built-in modules ────────────────────────────
// Only the VM runs the program. This class is no longer an interpreter: it holds the module
// table (stdlib + web core + what is `use`d), the output stream, the request context and the
// app:: service registry; module functions hand callbacks to the VM via invoke().

class RuntimeHost : public ModuleHost {
public:
    RuntimeHost();
    explicit RuntimeHost(std::ostream& out);
    void  set_web_context(WebContext* ctx);
    // Bind the web modules to the request at call time (look/request_scope.h): set up once,
    // each request supplies its own context via RequestScope. The web dispatch copy uses this.
    void  bind_web_to_current_request();
    void  set_output(std::ostream& out) { output_stream_ = &out; }
    void  set_output(std::ostream* out) { output_stream_ = out; }
    // Where the output of a callback (job handler, timer, parallel task) goes: on the command
    // line (output is std::cout) to stdout — OLD BUG: it was written to a separate buffer and discarded, a `print`
    // in a job script silently vanished. On the web a callback has no response to write to →
    // the buffer (as before).
    std::ostream& callback_output(const std::shared_ptr<std::ostringstream>& sink) const;
    std::ostream& output() const { return *output_stream_; }

    // Is the (top-level) setup in progress — db::query handles the side effect during setup accordingly.
    void set_setup_mode(bool m) { setup_mode_ = m; }
    bool is_setup_mode()  const override { return setup_mode_; }

    Value invoke(const Value& fn, std::vector<Value> args) override;
    Value jobs_run(int interval_ms);   // jobs::run — a single definition for web and the command line

    // runtime:: stats. The web engine writes the route count after setup (the VM's route table).
    void set_route_count(int n) { route_count_ = n; }
    int  get_route_count()   const override { return route_count_; }
    long get_uptime_sec()    const override {
        auto now = std::chrono::steady_clock::now();
        return (long)std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    }

    void set_file(const std::string& file) { current_file_ = file; if (main_script_.empty()) main_script_ = file; }

    // Thread copy: the same module state and service registry, its own output and context.
    // set_output() and set_web_context() are called on the copy.
    std::unique_ptr<RuntimeHost> make_dispatch_copy() const;

    // VM builtin wiring: take a function from modules_ after set_web_context().
    // module = "response", fn = "header" → modules_["response"].functions["header"]
    // Returns a null std::function if not found.
    NativeFn get_module_fn(const std::string& module_name, const std::string& fn_name) const;

    // Loads a stdlib module into modules_ (`use` semantics). If it is not in stdlib (a package module)
    // false — the compiler compiles the package module.
    bool load_stdlib_module(const std::string& name);
    // Is there a stdlib module that requires `use` but has not been `use`d? If so the VM gives
    // "Module 'x' not loaded." for that module's function — the binder leaves the slot empty. (Modules
    // that do not require `use` — web core, ws/sse … — return false here.)
    bool module_not_used(const std::string& name) const {
        return stdlib_.count(name) != 0 && modules_.count(name) == 0;
    }
    // All loadable modules (stdlib + web core): for the VM coverage report (lk --vm-report).
    const std::map<std::string, Module>& stdlib_modules() const { return stdlib_; }

private:
    std::ostream* output_stream_;
    std::map<std::string, Module> modules_;
    std::map<std::string, Module> stdlib_;

    // Service registry — app::set/get (written at setup, shared by all dispatches)
    std::shared_ptr<ServiceRegistry> services_ = std::make_shared<ServiceRegistry>();
    void register_app_module();   // called in the constructor — the app:: core module
    WebContext* web_ctx_ = nullptr;

    std::string main_script_;    // project root script — does not change
    std::string current_file_;
    bool        setup_mode_  = false;
    int         route_count_ = 0;

    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

// .env-aware env() — for the VM setup builtins (g_env_vars + system env)
std::string look_get_env(const std::string& key, const std::string& default_val = "");

} // namespace look

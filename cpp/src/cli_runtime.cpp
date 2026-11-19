// cli_runtime.cpp — command-line VM environment: builtin table + callback bindings.
// `lk` (main.cpp) and `lk test` (test_runner.cpp) use the SAME table — tests exercise the environment
// the program actually runs in (single engine, step 6).
#include "look/num_ops.h"   // abs/floor/ceil/round single definition
#include "look/cli_runtime.h"
#include "look/value_path.h"   // value_push_not_map — push-to-dictionary error (single definition)
#include "look/lexer.h"
#include "look/parser.h"
#include "look/capture_check.h"
#include "look/arg_check.h"
#include "look/runtime_host.h"
#include "look/array_count.h"
#include "look/http_client.h"
#include "look/runtime_init.h"    // runtime_init (CA probe + SQLite init, process start)
#include "look/web.h"
#include "look/parallel_runtime.h"
#include "look/ast.h"
#include "look/builtins.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/timer.h"         // TimerManager — sets up a timer:: VM closure on the command line
#include "look/vm_closure_runner.h" // vm_closure_runner — runs jobs::run handlers in the VM
#include <memory>
#include <unordered_map>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

namespace look {

// ── CLI-VM — builtin table for running a command-line / REPL / lk test script on the bytecode VM.
// Same semantics as http_main req_builtins; output goes to `out`.
// Module fns are auto-wired from the LOADED modules of RuntimeHost (requires use).
std::vector<look::BuiltinFn> build_cli_builtins(look::RuntimeHost& interp, std::ostream& out) {
    using look::Value;
    std::vector<look::BuiltinFn> b(look::builtin_names().size());
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    b[0] = [&out](std::vector<Value>& a) -> Value { for (auto& x : a) out << x.to_string(); return Value(); };
    b[1] = b[0];  // print/write
    b[2] = [](std::vector<Value>& a) -> Value {   // count/len — single definition (array_count.h)
        return Value(a.empty() ? 0 : look_count(a[0]));   // assoc: (size-1)/2 (sentinel skipped)
    };
    b[5] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? std::string() : a[0].to_string()); };  // str
    b[6] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); return look_explicit_int(a[0]); };  // int — to_int(): the stoll(to_string(float)) scientific-notation bug + int32 narrowing are closed
    b[7] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0.0); return look_explicit_float(a[0]); };  // float — to_float(): no to_string() round-trip
    b[8] = [](std::vector<Value>& a) -> Value {   // bool
        if (a.empty()) return Value(false);
        auto& v = a[0];
        if (v.type()==Value::BOOL) return v;
        if (v.type()==Value::INT)  return Value(v.as_int()!=0);
        if (v.type()==Value::FLOAT)return Value(v.as_float()!=0.0);
        if (v.type()==Value::STRING)return Value(!v.as_string().empty());
        if (v.type()==Value::NONE) return Value(false);
        return Value(true);
    };
    b[9] = b[5];  // string alias
    b[22] = [](std::vector<Value>&) -> Value { return Value(); };  // route — no dispatch in the CLI
    b[BI("strlen")] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? 0 : (int)a[0].to_string().size()); };
    b[BI("abs")] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); return num_abs(a[0]); };   // look/num_ops.h
    b[BI("max")] = [](std::vector<Value>& a) -> Value { for (auto& x : a) look::value_list_only(x, "max()"); return num_extreme(a, true); };    // look/num_ops.h
    b[BI("min")] = [](std::vector<Value>& a) -> Value { for (auto& x : a) look::value_list_only(x, "min()"); return num_extreme(a, false); };
    b[BI("sqrt")] = [](std::vector<Value>& a) -> Value { return Value(a.empty()?0.0:std::sqrt(a[0].to_float())); };
    b[BI("strtoupper")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::toupper((unsigned char)c); return Value(s); };
    b[BI("strtolower")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::tolower((unsigned char)c); return Value(s); };
    b[BI("push")] = [](std::vector<Value>& a) -> Value { if (a.size()<2||a[0].type()!=Value::ARRAY) throw std::runtime_error("push() requires array and value"); look::value_push_not_map(a[0], "push()"); a[0].as_array()->push_back(a[1]); return a[0]; };
    b[BI("pop")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) throw std::runtime_error("pop() requires array"); look::value_list_only(a[0], "pop()"); auto ar=a[0].as_array(); if (ar->empty()) return Value(); Value l=ar->back(); ar->pop_back(); return l; };
    b[BI("join")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) return Value(a.empty()?std::string():a[0].to_string()); look::value_list_only(a[0], "join()"); std::string sep=a.size()>=2?a[1].to_string():""; std::string r; auto& ar=*a[0].as_array(); for(size_t i=0;i<ar.size();++i){ if(i)r+=sep; r+=ar[i].to_string(); } return Value(r); };
    b[BI("stop")] = [](std::vector<Value>&) -> Value { return Value(); };
    // exit()/die() — were not bound in CLI-VM (they threw "Not callable").
    // Rule: exit code if the first argument is an INT, otherwise 0.
    {
        auto do_exit = [](std::vector<Value>& a) -> Value {
            int code = 0;
            if (!a.empty() && a[0].type() == Value::INT) code = (int)a[0].as_int();
            throw look::ExitException(code);
        };
        if (look::builtin_index("exit") >= 0) b[BI("exit")] = do_exit;
        if (look::builtin_index("die")  >= 0) b[BI("die")]  = do_exit;
    }
    b[BI("before_route")] = [](std::vector<Value>&) -> Value { return Value(); };
    b[BI("env")] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(); const char* e=std::getenv(a[0].to_string().c_str()); if (e) return Value(std::string(e)); return a.size()>=2?Value(a[1].to_string()):Value(std::string()); };
    if (look::builtin_index("args") >= 0)
        b[BI("args")] = [](std::vector<Value>&) -> Value {
            auto arr = std::make_shared<ValueArray>();
            for (const auto& s : look::script_args()) arr->push_back(Value(s));
            return Value(arr);
        };
    // channel() — was NOT BOUND in CLI-VM: send/receive have CHAN_* opcodes but
    // channel CREATION is a plain builtin call → empty in the VM table → an "unavailable
    // (not bound)" error. Since the old tree-walk handled it inline,
    // parallel+channel worked in tree-walk on the CLI but crashed in the VM (DEFAULT). Semantics:
    // default 128, n<0 error, n==0 → unbuffered ((size_t)-1).
    if (look::builtin_index("channel") >= 0)
        b[BI("channel")] = [](std::vector<Value>& a) -> Value { return Value(look::make_channel(a)); };   // value.h
    // config() — the SAME CLASS as channel: bound on the web (http_main), missing in CLI-VM →
    // tree-walk returned "def" while the VM gave "unavailable". Rule: "section.key" →
    // the "SECTION_KEY" env key, g_env_vars+getenv are consulted (look_get_env uses the SAME source).
    // Not found + no default → null (distinguished with a sentinel).
    if (look::builtin_index("config") >= 0)
        b[BI("config")] = [](std::vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            std::string dk = a[0].to_string();
            auto dot = dk.find('.');
            std::string ek = (dot != std::string::npos) ? (dk.substr(0, dot) + "_" + dk.substr(dot + 1)) : dk;
            std::transform(ek.begin(), ek.end(), ek.begin(), ::toupper);
            if (a.size() >= 2) return Value(look::look_get_env(ek, a[1].to_string()));
            static const std::string NF = std::string("\x01__look_config_nf__");
            std::string v = look::look_get_env(ek, NF);
            return v == NF ? Value() : Value(v);
        };
    // Module fns: every "mod::fn" in builtin_names → the LOADED module of RuntimeHost.
    // ONLY modules that were `use`d are auto-wired: string::/math::/array:: require `use`
    // (always-on ones like json:: are wired separately, index-based).
    // NO preload — otherwise a program without `use` would work.
    const auto& names = look::builtin_names();
    for (size_t i = 0; i < names.size(); ++i) {
        auto pos = names[i].find("::");
        if (pos == std::string::npos) continue;
        auto f = interp.get_module_fn(names[i].substr(0, pos), names[i].substr(pos + 2));
        if (f) b[i] = [f](std::vector<Value>& args) -> Value { std::vector<Value> aa = args; return f(aa); };
    }
    return b;
}

// ── On the command line, callbacks run in the VM too ────────────────────────────────────
// timer::after/every and the jobs::worker handler used to drop the whole script to tree-walk.
// Now each call runs in its own VM (like on the web: a new VM per request), with the same builtin table.
//   * the job handler runs inside jobs::run, on the SAME thread → it sees the main VM's CURRENT globals
//   * the timer runs on another thread → it sees a copy of the globals as of when it was set up
// Output goes to stdout. OLD BUG: the interpreter wrote the handler's/timer's output to a separate buffer
// and discarded it — `print` in a job script on the command line silently vanished.
look::Value run_cli_closure(const CliCallbacks& cc, const look::Value& fnv, std::vector<look::Value> args,
                                   const std::unordered_map<std::string, look::Value>* globals,
                                   const char* method, const char* what, bool& ok) {
    ok = false;
    look::WebContext ctx;
    ctx.method = method;
    try {
        look::VM sub(*cc.shared, std::cout);
        if (globals) sub.set_globals(*globals);
        else if (cc.main_vm) sub.set_globals(cc.main_vm->get_globals());
        sub.set_web_context(&ctx);
        look::Value r = sub.call_closure(*fnv.as_bytecode_fn(), std::move(args));
        std::cout.flush();
        ok = true;
        return r;
    } catch (const look::ExitException&) {
        throw;
    } catch (const std::exception& ex) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "VM", std::string(what) + " error: " + ex.what());
    } catch (...) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "VM", std::string(what) + " unknown error");
    }
    return look::Value();
}
void wire_cli_callbacks(std::vector<look::BuiltinFn>& b, look::RuntimeHost& interp,
                               std::shared_ptr<CliCallbacks> cc) {
    using look::Value;
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    // jobs::run calls handlers through vm_closure_runner (BYTECODE_FN).
    look::vm_closure_runner() = [cc](const Value& fn, std::vector<Value> args, const char* what, bool& ok) {
        return run_cli_closure(*cc, fn, std::move(args), nullptr, "__JOB__", what, ok);
    };
    b[BI("jobs::run")] = [&interp](std::vector<Value>& a) -> Value {
        return interp.jobs_run(a.empty() ? 5000 : (int)a[0].to_float());
    };
    if (auto f = interp.get_module_fn("jobs", "worker"))
        b[BI("jobs::worker")] = [f](std::vector<Value>& a) -> Value {
            std::vector<Value> aa = a;
            if (aa.size() >= 2 && aa[1].type() == Value::BYTECODE_FN) aa[1] = aa[1].clone_for_thread();
            return f(aa);
        };
    for (const char* name : {"timer::after", "timer::every"}) {
        const bool repeat = std::string(name) == "timer::every";
        const std::string nm = name;
        b[BI(name)] = [cc, repeat, nm](std::vector<Value>& a) -> Value {
            if (a.size() < 2) throw std::runtime_error(nm + "() takes 2 arguments");
            if (a[1].type() != Value::BYTECODE_FN) throw std::runtime_error(nm + "() second argument must be a function");
            // The timer runs on another thread: a copy of the globals as of now (no sharing).
            auto snap = std::make_shared<std::unordered_map<std::string, Value>>();
            if (cc->main_vm)
                for (const auto& [k, v] : cc->main_vm->get_globals()) snap->emplace(k, v.clone_for_thread());
            Value fnv = a[1].clone_for_thread();
            auto cb = [cc, fnv, snap]() {
                bool ok = false;
                run_cli_closure(*cc, fnv, {}, snap.get(), "__TIMER__", "timer callback", ok);
            };
            const int ms = (int)a[0].to_int();
            return Value(repeat ? look::TimerManager::instance().every(ms, std::move(cb))
                                : look::TimerManager::instance().after(ms, std::move(cb)));
        };
    }
    b[BI("timer::cancel")] = [](std::vector<Value>& a) -> Value {
        if (a.empty()) throw std::runtime_error("timer::cancel() takes 1 argument");
        look::TimerManager::instance().cancel((int)a[0].to_int());
        return Value();
    };
    // timer::wait([ms=5000]) — same form as parallel::wait: waits until one-shot timers have fired and
    // their callbacks have finished; true = all finished, false = timed out.
    b[BI("timer::wait")] = [](std::vector<Value>& a) -> Value {
        int ms = 5000;
        if (!a.empty() && a[0].type() == Value::INT) ms = (int)a[0].as_int();
        return Value(look::TimerManager::instance().wait_one_shots(ms));
    };
    // ws::/sse:: only make sense on a web app's connection route — explicit error.
    for (const auto& n : look::builtin_names()) {
        if (n.rfind("ws::", 0) != 0 && n.rfind("sse::", 0) != 0) continue;
        b[BI(n.c_str())] = [n](std::vector<Value>&) -> Value {
            throw std::runtime_error(n + "() can be used only in a web application (lk-fcgi), not on the command line");
        };
    }
}

} // namespace look

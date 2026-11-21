// LOOK Bytecode VM
//
// Dispatch loop: switch(opcode) → GCC/Clang jump table.
// Value type: look/value.h. Struct: Value::STRUCT (look/struct_types.h).

#include <atomic>
#include <climits>
#include "look/vm.h"
#include "look/index_error.h"
#include "look/fiber.h"         // Fiber::current — specific to the active VM fiber (active_vm_slot)
#include "look/bytecode.h"
#include "look/web.h"
#include "look/value_path.h"
#include "look/runtime_host.h"   // ExitException — CALL_BUILTIN (module NativeFn) may throw it
#include "look/parallel_runtime.h"
#include "look/logger.h"
#include "look/builtins.h"      // builtin_names() — module/function name in the error message
#include "look/int_overflow.h"  // i64_add_ovf — INT+INT arithmetic fast path
#include "look/route_params.h"   // route parameters by name
#include "look/stack_guard.h"   // look_native_stack_low — the actual remaining stack in a callback
#include "look/mem_budget.h"    // mem_check — memory budget at a safe point
#include <algorithm>

#include <sstream>
#include <cmath>
#include <cassert>
#include <thread>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>
#include <mutex>
#include <set>

namespace {
// LOOK_WARN_UNDEF=1 → log undefined-variable reads (does NOT CHANGE behavior).
// Read once; checked only on LOAD_GLOBAL's miss branch → no cost on the hot path.
const bool g_warn_undef = [] {
    const char* e = std::getenv("LOOK_WARN_UNDEF");
    return e && e[0] == '1';
}();
} // namespace

namespace look {

// The VM's safe point (backward jump, call): yielding (fiber.h preempt_point) + the memory budget
// (look/mem_budget.h). On the fast path a single counter decrement; an allocation that exceeds the budget resets the counter, the slow path
// checks here right away. preempt_point is noexcept — a memory error can only be thrown from here.
// The fast path is forced inline and the work is out of line: as two plain `static inline` functions both
// stayed out-of-line calls on every call and every loop turn (callgrind, 300k calls: vm_safe_point and
// check_argc 9.6M instructions each, about half of the +10% against 1.0).
#if defined(_MSC_VER)
#  define LOOK_VM_HOT_INLINE __forceinline
#  define LOOK_VM_SLOW __declspec(noinline)
#else
#  define LOOK_VM_HOT_INLINE inline __attribute__((always_inline))
#  define LOOK_VM_SLOW __attribute__((noinline, cold))
#endif
LOOK_VM_SLOW static void vm_safe_point_slow() { preempt_slow(); mem_check(); }
static LOOK_VM_HOT_INLINE void vm_safe_point() {
    if (--t_preempt_budget <= 0) vm_safe_point_slow();
}

// parallel transitive deep-clone hook. value.h cannot see Closure (bytecode.h
// defines it later) → when deep_clone_impl sees BYTECODE_FN it calls the cloner registered here.
// When a Closure is cloned, the values it captured AND inner closures are cloned recursively; otherwise
// the array/map inside a closure captured by a parallel task stays shared with the parent
// (np1 data race). Channel/scalar captures stay shared (deep_clone_impl *this).
static Value clone_bytecode_fn(const Value& v, std::unordered_set<const void*>& visited) {
    auto src = v.as_bytecode_fn();
    if (!src) return v;
    if (visited.count(src.get())) return Value(); // break the cycle
    visited.insert(src.get());
    auto nc = std::make_shared<Closure>(src->proto);
    nc->native   = src->native;                    // host function (if any) as is
    nc->captures = src->captures;                  // shallow; below, non-handles are deep-copied
    // R-02: capture isolation of a closure given to a parallel task — DENY LIST
    // (fail-safe) instead of an ALLOW LIST (fail-open). Every capture is deep_cloned EXCEPT
    // handle types whose sharing is INTENTIONAL (channel/ws/sse — cross-thread communication primitives).
    // So a plain ARRAY capture like `use ($array)` is isolated too (the old state only
    // cloned inner closures; a plain array was SHARED through a shallow shared_ptr and parallel tasks
    // mutated the same vector concurrently and RACED — t1, TSan). Critical: when a new type is added to
    // Value it starts automatically on the SAFE side — with an allow list every
    // new type would be a silent-sharing bug (the 3rd patch of this class). deep_clone_impl handles every type
    // correctly: ARRAY recursive clone, closure clone via the hook, scalar/string *this (cheap,
    // immutable).
    for (size_t j = 0; j < nc->captures.size(); ++j) {
        Value::Type t = nc->captures[j].type();
        bool shared_by_design = (t == Value::CHANNEL || t == Value::WEBSOCKET
                              || t == Value::SSE_CONN);
        if (!shared_by_design)
            nc->captures[j] = nc->captures[j].deep_clone_tracked(visited);
    }
    visited.erase(src.get());
    return Value(nc);
}
static const bool s_bc_cloner_registered = [] {
    Value::bc_fn_cloner() = &clone_bytecode_fn;
    return true;
}();

// ── VM callback bridge (hook side) ─────────────────────────────────────────
// The bridge functions are in runtime_host.cpp; here the VM
// only registers its hook. The active VM is registered inside run(): inside a fiber it is SPECIFIC TO THE FIBER
// (FiberLocal::active_vm), outside a fiber specific to the thread. It used to be only thread_local:
// when a request was suspended in a query / http::get, another request's
// VM on the same thread overwrote it; the array::map closure of the resumed request ran in the OTHER request's VM,
// read its header/parameter, and then the process crashed (1.0.20–1.0.23, fiber mode).
// fiber-ok: only a backup outside a fiber; inside a fiber FiberLocal::active_vm
static thread_local VM* t_active_vm = nullptr;
static VM*& active_vm_slot() {
    if (Fiber* f = Fiber::current()) return reinterpret_cast<VM*&>(f->local.active_vm);
    return t_active_vm;
}

// ── argument count ───────────────────────────────────────────────────────────
// A user call and a runtime callback must match exactly: both an extra argument and
// a missing parameter that has no default are an ERROR (the interpreter always did this; the VM silently
// accepted it, a missing parameter became null). The messages are the same as the old interpreter's.
// Route dispatch does NOT pass through this check: the handler takes as many of the route parameters as it wants.
[[noreturn]] LOOK_VM_SLOW static void check_argc_fail(const FunctionProto* p, int argc) {
    const int fixed = p->variadic ? p->arity - 1 : p->arity;
    const std::string nm = (p->name.empty() || p->name == "<closure>") ? "__anonymous__" : p->name;
    if (!p->variadic && argc > fixed)
        throw LookVmError("Function '" + nm + "' expects at most " + std::to_string(fixed)
                          + " args, got " + std::to_string(argc));
    throw LookVmError("Function '" + nm + "' expects " + std::string(p->variadic ? "at least " : "")
                      + std::to_string(fixed) + " args, got " + std::to_string(argc));
}
static LOOK_VM_HOT_INLINE void check_argc(const FunctionProto* p, int argc) {
    const int fixed = p->variadic ? p->arity - 1 : p->arity;
    if (argc >= p->required && (p->variadic || argc <= fixed)) return;
    check_argc_fail(p, argc);
}

static Value vm_bridge_hook(const Value& fn, std::vector<Value>& args) {
    VM* vm = active_vm_slot();
    if (!vm)
        throw LookVmError("vm_bridge_hook: no active VM");
    if (fn.type() != Value::BYTECODE_FN)
        throw LookVmError("vm_bridge_hook: BYTECODE_FN expected");
    check_argc(fn.as_bytecode_fn()->proto.get(), (int)args.size());   // callback: the count must match exactly
    return vm->call_closure(*fn.as_bytecode_fn(), args);
}

// ── VM ctor ───────────────────────────────────────────────────────────────

VM::VM(SharedState shared, std::ostream& output)
    : shared_(shared), output_(output)
{
    regs_.reserve(1024);
    call_stack_.reserve(64);
}

void VM::set_globals(std::unordered_map<std::string, Value> g) {
    globals_ = std::move(g);
    global_cache_.clear();   // A2: globals_ changed entirely → cached Value*'s are invalid
}
void VM::set_web_context(WebContext* ctx)                        { web_ctx_ = ctx; }
void VM::set_ws_connection(std::shared_ptr<WsConnection> ws)    { ws_conn_ = ws; }
void VM::set_sse_connection(std::shared_ptr<SseConnection> sse) { sse_conn_ = sse; }

// ── execute ───────────────────────────────────────────────────────────────────

Value VM::execute(const CompiledProgram& prog) {
    auto cl = std::make_shared<Closure>(prog.main_proto);
    push_frame(cl.get(), prog.main_proto->reg_count, -1);
    return run();
}

// ── call_closure ──────────────────────────────────────────────────────────────

Value make_native_fn(const std::string& name, BuiltinFn fn) {
    auto p = std::make_shared<FunctionProto>();
    p->name     = name;
    p->variadic = true;   // accepts any argument count — the function itself does the check
    p->arity    = 1;
    p->required = 0;
    p->reg_count = 0;
    auto cl = std::make_shared<Closure>(p);
    cl->native = std::move(fn);
    return Value(cl);
}

// Rare paths are out of line: embedding them in run() grew its C++ frame and increased the stack share
// of every callback level (objdump: run() 0x218 → 0x4e8).
#if defined(_MSC_VER)
#  define LOOK_VM_COLD __declspec(noinline)
#else
#  define LOOK_VM_COLD __attribute__((noinline, cold))
#endif
// ── Stack limits (explanation of MAX_* in look/vm.h) ──────────────────────
[[noreturn]] LOOK_VM_COLD static void stack_overflow_calls() {
    throw LookVmError("Stack overflow: more than 10000 nested calls (endless recursion?)");
}
[[noreturn]] LOOK_VM_COLD static void stack_overflow_memory() {
    throw LookVmError("Stack overflow: the call stack is larger than 8 MB (endless recursion?)");
}
[[noreturn]] LOOK_VM_COLD static void stack_overflow_callbacks() {
    throw LookVmError("Stack overflow: more than 4000 nested callbacks (a function given to "
                      "map/filter/sort/... that calls itself through it)");
}
[[noreturn]] LOOK_VM_COLD static void stack_overflow_native() {
    throw LookVmError("Stack overflow: the native stack is nearly full (too many nested callbacks)");
}

// Error path (cold): the frame of run() = the BOTTOM-MOST frame whose base is `base` and proto is `proto`.
// The ones above it (left by nested calls) are those with base ≥ base; stop when you
// reach one with a smaller base. If not found the line is not recorded (stays 0 as before).
LOOK_VM_COLD void VM::note_error_line_for(int base, const FunctionProto* proto) {
    const Frame* found = nullptr;
    for (size_t i = call_stack_.size(); i-- > 0;) {
        const Frame& f = call_stack_[i];
        if (f.base < base) break;
        if (f.base == base && f.proto == proto) found = &f;
    }
    if (found) note_error_line(*found, proto);
}

// Call chain: call_stack_[k] (k ≥ 1) is a call — its name is its own proto's, its line is the caller's
// (call_stack_[k-1]) current instruction (ip-1 = the call instruction; in a callback the line
// where a built-in like array::map was called). Same as the old interpreter: innermost first. In deep recursion
// (10 000 calls) the innermost and outermost 50 are written, one line in between — Go also skips the middle.
// `$l = array::push($l, $x)` / `$l = array::slice($l, 0, -1)` are compiled in place and skip the call;
// it worked without `use array` too (the path form gives "Module 'array' not loaded."). One release
// a warning, then an error (same schedule as push/pop). Once per place (process lifetime) — so the loop does not fill the log.
static LOOK_VM_COLD void warn_in_place_without_use_array(const FunctionProto* proto, int ip, const char* fn) {
    static std::mutex mu;
    static std::set<std::pair<const FunctionProto*, int>> seen;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!seen.insert({proto, ip}).second) return;
    }
    int line = 0;
    std::string file;
    if (proto && ip >= 0 && ip < (int)proto->lines.size()) { line = proto->lines[ip]; file = proto->file_at(ip); }
    // "" = main script (bytecode.h file_runs) → line only; in an included file file:line
    std::string where = line <= 0 ? std::string()
                      : file.empty() ? "line " + std::to_string(line) + ": " : file + ":" + std::to_string(line) + ": ";
    Logger::instance().log(LogLevel::LOG_WARN, "check",
        where + std::string(fn) + " is used without `use array`; it runs only because this form skips the "
        "module check (add `use array`; this will be an error in a later release; lk fix adds it)");
}

LOOK_VM_COLD void VM::capture_error_stack() {
    last_error_stack_.clear();
    const size_t n = call_stack_.size();
    if (n < 2) return;
    constexpr size_t kEdge = 50;
    auto add = [&](size_t k) {
        StackFrame sf;
        const Frame& callee = call_stack_[k];
        const Frame& caller = call_stack_[k - 1];
        sf.function = callee.proto ? callee.proto->name : std::string();
        // A command without a line (e.g. a call in the body of fn($v) => f($v)) → the nearest previous line.
        int i = caller.ip - 1;
        if (caller.proto) {
            const auto& ln = caller.proto->lines;
            if (i >= (int)ln.size()) i = (int)ln.size() - 1;
            while (i >= 0 && ln[i] <= 0) --i;
            if (i >= 0) { sf.line = ln[i]; sf.file = caller.proto->file_at(i); }
        }
        last_error_stack_.push_back(std::move(sf));
    };
    const size_t calls = n - 1;
    if (calls <= 2 * kEdge) {
        for (size_t k = 1; k < n; ++k) add(k);
        return;
    }
    for (size_t k = 1; k <= kEdge; ++k) add(k);
    StackFrame gap;
    gap.line = -1;
    gap.function = "... " + std::to_string(calls - 2 * kEdge) + " more calls ...";
    last_error_stack_.push_back(std::move(gap));
    for (size_t k = n - kEdge; k < n; ++k) add(k);
}

// parallel() task: is it a built-in that leaves a trace outside (effect class Effect or unclassified)?
// Pure/Reads/ThrowCheck/Callback also run after the script ends (parallel_runtime.h).
static const std::vector<char>& builtin_leaves_trace() {
    static const std::vector<char> t = [] {
        const auto& names = builtin_names();
        std::vector<char> v(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            const BuiltinEffect e = builtin_effect(names[i]);
            v[i] = (e == BuiltinEffect::Effect || e == BuiltinEffect::Unknown);
        }
        return v;
    }();
    return t;
}
// End of the task's effect point: when the built-in/channel returns (or throws) RUNNING again.
struct TaskEffectEnd {
    TaskExit* t;
    ~TaskEffectEnd() { if (t) task_effect_end(t); }
};

LOOK_VM_COLD void VM::reserve_regs(size_t n) {
    static_assert(MAX_CALL_DEPTH == 10000 && MAX_NATIVE_DEPTH == 4000 &&
                  MAX_STACK_VALUES * sizeof(Value) == 8u * 1024 * 1024,
                  "the Stack overflow messages above name these limits: update them together");
    if (n > MAX_STACK_VALUES) stack_overflow_memory();
    // Grow to double but do not exceed the budget: the temporary allocation (old + new) stays at most the budget.
    regs_.reserve(std::min(MAX_STACK_VALUES, std::max(n, regs_.capacity() * 2)));
}

Value VM::call_closure(const Closure& closure, std::vector<Value> args) {
    if (closure.native) return closure.native(args);   // host function
    if ((int)call_stack_.size() >= MAX_CALL_DEPTH) stack_overflow_calls();
    // Callback nesting: every level uses C++ stack (run() re-entry). The counter gives the same behavior in every
    // environment, and the real stack check prevents crashing even on a small stack.
    if (native_depth_ >= MAX_NATIVE_DEPTH) stack_overflow_callbacks();
    if (look_native_stack_low()) stack_overflow_native();
    struct NativeDepth { int& d; explicit NativeDepth(int& x) : d(x) { ++d; } ~NativeDepth() { --d; } } nd(native_depth_);

    auto* proto = closure.proto.get();
    int base = push_frame(&closure, proto->reg_count, -1);

    // Write the arguments to registers
    int argc = (int)args.size();
    call_stack_.back().argc = argc;   // for the default-param prologue
    if (!proto->variadic) {
        for (int i = 0; i < proto->arity && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
    } else {
        int fixed = proto->arity - 1;
        for (int i = 0; i < fixed && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
        // Variadic: remaining args → ARRAY
        auto varr = std::make_shared<ValueArray>();
        for (int i = fixed; i < argc; ++i) varr->push_back(std::move(args[i]));
        if (fixed >= 0 && proto->arity > 0)
            regs_[base + fixed] = Value(varr);
    }

    return run();
}

// ── push_frame ────────────────────────────────────────────────────────────

int VM::push_frame(const Closure* cl, int reg_count, int ret_reg) {
    int base = (int)regs_.size();
    grow_regs((size_t)base + reg_count);
    call_stack_.push_back({cl->proto.get(), cl, 0, base, ret_reg});
    return base;
}


// ── dispatch_routes ───────────────────────────────────────────────────────

void VM::dispatch_routes(const std::string& method, const std::string& path) {
    if (!shared_.routes) return;
    for (auto& entry : *shared_.routes) {
        size_t colon = entry.pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = entry.pattern.substr(0, colon);
        std::string p = entry.pattern.substr(colon + 1);
        if (m != method && m != "*") continue;
        std::vector<Value> params;
        if (route_match(p, path, params)) {
            // Run the route-level middlewares
            bool stopped = false;
            for (auto* mw : entry.middlewares) {
                try { call_closure(*mw, {}); }
                catch (const RouteStopException&) { stopped = true; break; }
            }
            if (!stopped) {
                // route parameters are passed to the handler BY NAME (look/route_params.h).
                std::vector<Value> named_args;
                // On a WS / SSE route the first parameter is the connection.
                if (m == "WS" && ws_conn_)        named_args.push_back(Value(ws_conn_));
                else if (m == "SSE" && sse_conn_) named_args.push_back(Value(sse_conn_));
                if (web_ctx_) look::route_args_by_name(m, p, entry.fn->proto->params, named_args.size(), web_ctx_->route_params, named_args);
                else          named_args = std::move(params);
                call_closure(*entry.fn, std::move(named_args));
            }
            return;
        }
    }
    // No matching route → status 404, set BEFORE THE HANDLER.
    // OLD BUG: the status was set only in the default body; if the application defined route("404", ...)
    // the handler was called and returned, and the custom 404 page went out with 200.
    // The handler can change it with response::status if it wants.
    if (web_ctx_) { web_ctx_->status_code = 404; web_ctx_->status_text = "Not Found"; }
    for (auto& entry : *shared_.routes) {
        size_t colon = entry.pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = entry.pattern.substr(0, colon);
        if (m == "404") {
            call_closure(*entry.fn, {});
            return;
        }
    }
    output_ << "404 Not Found";
}

bool VM::route_match(const std::string& pattern, const std::string& path,
                     std::vector<Value>& params) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream ss(s);
        std::string t;
        while (std::getline(ss, t, '/')) if (!t.empty()) v.push_back(t);
        return v;
    };
    auto pp = split(pattern), rp = split(path);
    if (pp.size() != rp.size()) return false;
    // Collect the named params; write to web_ctx_ only on a FULL match (a partial
    // match must not pollute route_params).
    std::vector<std::pair<std::string, std::string>> named;
    for (size_t i = 0; i < pp.size(); ++i) {
        if (pp[i].size() > 2 && pp[i].front() == '{' && pp[i].back() == '}') {
            // URL-decode the path segment (query params already are; path params were not —
            // /p/%C3%BC gave "%C3%BC" not "ü"). plus_is_space=false: '+' is literal in a path.
            std::string dv = WebContext::url_decode(rp[i], false);
            params.push_back(Value(dv));
            named.emplace_back(pp[i].substr(1, pp[i].size() - 2), dv);
        } else if (pp[i] != rp[i]) return false;
    }
    // so that request::param("id") / route::param("id") work on the VM path too
    // (the old interpreter filled route_params — the divergence is fixed).
    if (web_ctx_)
        for (auto& [k, v] : named) web_ctx_->route_params[k] = v;
    return true;
}

// ── Value helpers ─────────────────────────────────────────────────────────

// Is it a WHOLE-NUMBER text like "12" / "-3"? To tell whether an array key is a numeric index or
// an assoc key (single definition: look/value_path.h).
static bool look_is_int_key(const std::string& s) { return look::value_is_int_key(s); }

bool VM::val_truthy(const Value& v) { return v.is_truthy(); }

std::string VM::val_to_str(const Value& v) { return v.to_string(); }

// look/struct_types.h throws std::runtime_error; VM errors are carried as LookVmError.
template <class F> static auto vm_struct(F&& f) -> decltype(f()) {
    try { return f(); }
    catch (const LookVmError&) { throw; }
    catch (const std::runtime_error& e) { throw LookVmError(e.what()); }
}

Value VM::array_get(const Value& arr, const Value& key) {
    // struct field ($u.age, $u["age"]). A struct is not an array: there is no numeric index.
    if (arr.type() == Value::STRUCT) {
        if (key.type() != Value::STRING)
            throw LookVmError("a struct is not an array: use a field name ('"
                              + look::struct_name(*arr.as_struct()) + "' has no numeric index)");
        return vm_struct([&]() -> Value { return look::struct_get_named(*arr.as_struct(), key.str_ref()); });
    }
    // indexing a non-array value is an error (look/index_error.h).
    // It used to silently return null.
    if (arr.type() == Value::NONE) return Value();   // null reads like an empty map (Go: reading a nil map is not an error)
    if (arr.type() != Value::ARRAY) throw LookVmError(look::index_error_message(arr, key));
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, k1, v1, ...] — sentinel at position 0
    // str_ref(): behind a string pointer since B5 — as_string()/to_string() return a
    // copy on every access. Sentinel and key comparisons are copy-free (object-lookup hot path).
    if (!vec.empty() && vec[0].is_assoc_marker()) {
        // Copy-free lookup key: a STRING key (hot path, $row["name"]) goes directly through
        // str_ref(); a numeric key ($a[5]) is still coerced to "5" (semantics preserved).
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref()
                                                             : (ktmp = key.to_string());
        // Indexed on a large map (look/assoc_index.h); linear on a small one and on the old __struct__ tag.
        size_t pos = look::assoc_find(vec, k);
        return pos ? vec[pos + 1] : Value();
    }
    // List + a non-integer string key: there is no such key (same contract as
    // a key not found in an assoc). The $obj.field access falls here too.
    if (key.type() == Value::STRING && !look_is_int_key(key.str_ref())) return Value();

    // Numeric index — REFERENCE SEMANTICS (same as the old tree-walk):
    // negative = from the end, out of range = ERROR.
    // OLD BUG: the VM SILENTLY returned null when out of range and for negatives, while the
    // interpreter threw an error/counted from the end — the same program gave two
    // different results on two engines:
    //   $a=[1,2,3]; $a[99]  ->  VM null   / tree-walk ERROR
    //   $a=[1,2,3]; $a[-1]  ->  VM null   / tree-walk 3 (from the end)
    // A silent null is the very class that hides wrong data (same reasoning as the undefined-variable
    // STRICT decision): an access that exceeds the bound in a loop must give an error.
    int64_t i = key.to_int();
    if (i < 0) i += (int64_t)vec.size();
    if (i < 0 || i >= (int64_t)vec.size())
        throw LookVmError("Array index " + std::to_string(key.to_int()) + " out of bounds");
    return vec[(size_t)i];
}

// ── copy-on-write path assignment (rules: look/value_path.h) ──
Value* VM::array_slot(Value& arr, const Value& key) {
    return vm_struct([&] { return look::value_slot(arr, key); });
}
void VM::set_path(Value& slot, const Value* keys, int n, const Value& val) {
    vm_struct([&] {
        look::value_set_path(slot, keys, n, [&](Value& c, const Value& k, const Value* forced) {
            array_set(c, k, forced ? *forced : val);
        });
    });
}

void VM::array_set(Value& arr, const Value& key, const Value& val) {
    if (arr.type() == Value::NONE) {
        arr = Value(std::make_shared<ValueArray>());
    }
    // LOOK strings are immutable (there is no string-mutation builtin, and index assignment
    // has no write path), but the VM used to SILENTLY ignore `$s[0] = "x"` — the silent-wrong
    // class. The tree-walk interpreter threw here; the VM now does too, with the same
    // message (tests grep it). (Also locks the
    // precondition for the string-view representation: strings never change under a view.)
    if (arr.type() == Value::STRING)
        throw LookVmError("Strings are immutable; cannot assign to a string index");
    // writing to a struct field — fixed shape, type-checked. Literal construction (SET_FIELD)
    // also goes through here; the rules are in look/struct_types.h.
    if (arr.type() == Value::STRUCT) {
        if (key.type() != Value::STRING)
            throw LookVmError("a struct is not an array: use a field name ('"
                              + look::struct_name(*arr.as_struct()) + "' has no numeric index)");
        vm_struct([&] { look::struct_set_named(*arr.as_struct(), key.str_ref(), val); });
        return;
    }
    if (arr.type() != Value::ARRAY) return;
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, ...] — sentinel at position 0
    bool is_assoc = !vec.empty() && vec[0].is_assoc_marker();
    // A TEXT like "12"/"-3" is a numeric index, not an assoc key
    // (the interpreter always counted these texts as a numeric index — the engines diverged).
    bool str_key = key.type() == Value::STRING && !look_is_int_key(key.str_ref());
    if (is_assoc || str_key) {
        // Copy-free lookup key (same as array_get): a STRING key via str_ref();
        // a numeric key is coerced to "5". A new entry Value(k) copies again.
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref()
                                                             : (ktmp = key.to_string());
        // OLD BUG: while the list was NOT yet an assoc, the loop below took it for a pair list
        // and scanned it (start=0), then only prepended the sentinel:
        //   $a=[1,2,3]; $a["k"]="X"  ->  ["__assoc__",1,2,3,"k","X"]
        // read as pairs this gives 1->2, 3->"k": ALL DATA DISAPPEARS and the new
        // value is unreachable too. First convert by keying the elements with their NUMERIC INDEXES
        // (same contract as array::set).
        if (!is_assoc) {
            std::vector<Value> conv;
            conv.reserve(vec.size() * 2 + 1);
            conv.push_back(Value::assoc_marker());
            for (size_t i = 0; i < vec.size(); ++i) {
                conv.push_back(Value(std::to_string(i)));
                conv.push_back(vec[i]);
            }
            vec.swap(conv);
        }
        // Existing key: replace the value in place (indexed lookup on a large map).
        if (size_t pos = look::assoc_find_for_write(vec, k)) { vec[pos + 1] = val; return; }
        // New entry — append key, value at end. When the key is already a STRING Value,
        // store it directly (shared_ptr copy) instead of re-materializing it with a fresh
        // make_shared<string> — the key literal already lives in the constant pool, so
        // Value(k) was allocating a duplicate of a constant on every insert (5 per assoc in
        // object_create). Only a coerced numeric key ("5") needs a freshly built string.
        if (key.type() == Value::STRING) vec.push_back(key);
        else                             vec.push_back(Value(k));
        vec.push_back(val);
        look::assoc_appended(vec);
        return;
    }
    // Numeric index — REFERENCE SEMANTICS (same as the old tree-walk):
    // negative = from the end, equal to size = append, anything else is an ERROR.
    // OLD BUG: the VM SILENTLY SKIPPED THE WRITE when out of range and for negatives —
    // the assignment did nothing and gave no error either:
    //   $a=[1,2,3]; $a[99]="X"  ->  VM [1,2,3] (the assignment vanished) / tree-walk ERROR
    //   $a=[1,2,3]; $a[-1]="X"  ->  VM [1,2,3] (the assignment vanished) / tree-walk [1,2,"X"]
    // "I wrote it but it was not written" is the most insidious class: data loss, no error.
    int64_t idx = key.to_int();
    if (idx < 0) idx += (int64_t)vec.size();
    if (idx == (int64_t)vec.size()) { vec.push_back(val); return; }
    if (idx < 0 || idx > (int64_t)vec.size())
        throw LookVmError("Array index " + std::to_string(key.to_int()) + " out of bounds");
    vec[(size_t)idx] = val;
}

Value VM::get_field(const Value& obj, const std::string& field) {
    return array_get(obj, Value(field));
}

void VM::set_field(Value& obj, const std::string& field, const Value& val) {
    array_set(obj, Value(field), val);
}

// ── run — dispatch loop ───────────────────────────────────────────────────

Value VM::run() {
    // Callback bridge: register this VM as active (restore with RAII). A nested
    // run() sets the same VM — harmless. Built-ins like array::map run the callback
    // through RuntimeHost::invoke → vm_bridge_invoke → this VM's call_closure.
    // The slot is taken once on entry: during run() we stay in the same fiber, the restore writes to the same slot.
    VM*& _active = active_vm_slot();
    VM* _prev_active = _active;
    _active = this;
    register_vm_bridge(&vm_bridge_hook);   // register the thread-local hook (idempotent)
    struct ActiveGuard { VM*& slot; VM* prev; ~ActiveGuard() { slot = prev; } } _ag{_active, _prev_active};

    // Try base: this run() uses only the handlers it pushed itself. A nested
    // run() (a closure call coming from the bridge) cannot jump to an outer handler → it propagates
    // across the C++ boundary with LookVmThrow. On exit the base and leftover handlers are restored.
    struct FloorGuard {
        VM* vm; size_t prev_floor; size_t entry_size;
        ~FloorGuard() {
            if (vm->try_stack_.size() > entry_size) vm->try_stack_.resize(entry_size);
            vm->try_floor_ = prev_floor;
        }
    } _fg{this, try_floor_, try_stack_.size()};
    try_floor_ = try_stack_.size();

call_dispatch:
    while (!call_stack_.empty()) {
        Frame& frame = call_stack_.back();
        // The error path does NOT USE the `frame` reference: when a callback (map/sort… → call_closure) re-enters
        // the VM and grows call_stack_, the vector moves and the reference points to freed
        // memory (ASan: heap-use-after-free). The frame is found again by (base, proto)
        // (note_error_line_for). An extra live variable (an index) was tried: it grew run()'s C++ frame
        // from 0x218 to 0x4e8 and made every callback level more expensive (measured).
        const FunctionProto* proto = frame.proto;
        int base = frame.base;

        // A2: this proto's global inline cache vector (once on frame entry — frames change
        // far less often than instructions). An unordered_map rehash does not invalidate a mapped-value
        // reference → the gcache pointer is stable for the whole frame.
        std::vector<Value*>* gcache = &global_cache_[proto];
        if (gcache->size() != proto->constants.size())
            gcache->assign(proto->constants.size(), nullptr);
        // per-instruction cache for struct field access (definition pointer → slot).
        // Lazy: a function that does not touch a struct field never allocates it.
        std::vector<FieldCache>* fcache_p = nullptr;
        auto field_cache = [&]() -> std::vector<FieldCache>& {
            if (!fcache_p) {
                fcache_p = &field_cache_[proto];
                if (fcache_p->size() != proto->code.size()) fcache_p->assign(proto->code.size(), FieldCache{});
            }
            return *fcache_p;
        };

        // the root of path assignments — the variable's OWN place (see SET_PATH/PUSH_PATH).
        //   0 local · 4 global (a captured variable cannot be written: the compiler rejects it)
        auto path_root = [&](const Instruction& h) -> Value* {
            const uint32_t ix = h.bx();
            switch (h.a) {
                case 0: return &regs_[(size_t)(base + ix)];
                default: throw LookVmError("internal: unexpected path root kind");
                case 4: {
                    const std::string& gname = proto->constants[ix].str_ref();
                    auto it = globals_.find(gname);
                    if (it == globals_.end()) throw LookVmError("Undefined variable: " + gname);
                    return &it->second;
                }
            }
        };
        try {
        while (frame.ip < (int)proto->code.size()) {
            const Instruction& ins = proto->code[frame.ip++];

// A1: Hot-path register access. Since the compiler guarantees register indexes at compile time,
// the bounds check is dead cost in release → operator[]. In debug/ASan
// builds .at() is kept (if a bytecode generation bug overflows it is caught, no silent UB).
#ifdef NDEBUG
#define R(x) regs_[(size_t)(base + (x))]
#else
#define R(x) regs_.at((size_t)(base + (x)))
#endif
#define CONST(i) proto->constants[i]

            switch (ins.op) {

            // ── Load ──────────────────────────────────────────────────────────
            case OpCode::LOAD_CONST:
                R(ins.a) = CONST(ins.b);
                break;
            case OpCode::LOAD_CONST_W:
                R(ins.a) = CONST(ins.bx());
                break;
            case OpCode::LOAD_NULL:  R(ins.a) = Value();        break;
            case OpCode::LOAD_TRUE:  R(ins.a) = Value(true);    break;
            case OpCode::LOAD_FALSE: R(ins.a) = Value(false);   break;
            case OpCode::LOAD_INT:   R(ins.a) = Value((int)(int16_t)ins.b); break;
            case OpCode::MOVE:       R(ins.a) = R(ins.b);       break;
            case OpCode::LOAD_VAR:   R(ins.a) = R(ins.b);       break;
            case OpCode::STORE_VAR:  R(ins.a) = R(ins.b);       break;

            case OpCode::LOAD_GLOBAL: {
                uint32_t ni = ins.bx();
                // A2 inline cache: if there is a slot pinned for this ni, read it directly (register speed).
                if (Value* hit = (*gcache)[ni]) { R(ins.a) = *hit; break; }
                // str_ref(): avoid the as_string() copy (behind a string pointer since B5;
                // every global access would have had to pay a string copy). find takes const&.
                const std::string& gname = CONST(ni).str_ref();
                auto it = globals_.find(gname);
                if (it != globals_.end()) {
                    // an array coming from setup is SHARED with the request — no copy. If the request writes to it, the
                    // write path (SET_PATH / PUSH_PATH / detach) takes its own copy first.
                    (*gcache)[ni] = &it->second;   // pin the slot — later accesses without a miss
                    R(ins.a) = it->second;
                } else if (!gname.empty() && gname[0] == '$') {
                    // Undefined VARIABLE — STRICT (the tests grep the message).
                    // Reasoning: both ends of the target band are strict (Go: compile error,
                    // Node: ReferenceError); from PHP only the distribution/audience is taken, not the
                    // semantics. The interpreter (the reference engine at the time) was already strict — the deviating side
                    // was the VM, which produced a silent null (all the bugs of this round were of that class).
                    // LOOK_WARN_UNDEF=1 → TRANSITION MODE: the old lenient behavior + a warning
                    // (for migration only, not for permanent use).
                    if (g_warn_undef) {
                        R(ins.a) = Value();
                        Logger::instance().log(LogLevel::LOG_WARN, "VM",
                            "Undefined variable read (LOOK_WARN_UNDEF transition mode, returned null): " + gname);
                    } else {
                        throw LookVmError("Undefined variable: " + gname);
                    }
                } else {
                    // names that are not variables like "mod::fn": a miss here is NORMAL
                    // (falls to the general CALL path) → throwing would be wrong.
                    R(ins.a) = Value();
                }
                break;
            }
            case OpCode::STORE_GLOBAL: {
                // a=src_reg, b=name_hi, c=name_lo (16-bit constant pool index)
                uint32_t ni = ins.bx();
                // A2: if there is a pinned slot write directly (no hash). Writes ONLY to an EXISTING slot
                // (the cache is filled after the first find/insert → always a valid address).
                if (Value* hit = (*gcache)[ni]) { *hit = R(ins.a); break; }
                const std::string& name = CONST(ni).str_ref();  // copy-free
                Value& slot = globals_[name];   // create if missing, find if present
                slot = R(ins.a);
                (*gcache)[ni] = &slot;          // pin for later writes/reads
                break;
            }
            case OpCode::APPEND_GLOBAL: {
                // `$s .= x` / `$s = $s . x` at top level: append in place to the global's OWN slot (amortized
                // O(1); if shared, append_in_place copies once). OLD: read → new text → write,
                // a copy of the whole text at every step (quadratic in a loop).
                uint32_t ni = ins.bx();
                Value* slot = (*gcache)[ni];
                if (!slot) {
                    const std::string& gname = CONST(ni).str_ref();
                    auto it = globals_.find(gname);
                    if (it == globals_.end()) {
                        // same rule as LOAD_GLOBAL: STRICT; in transition mode it starts from null (with a warning)
                        if (!g_warn_undef) throw LookVmError("Undefined variable: " + gname);
                        Logger::instance().log(LogLevel::LOG_WARN, "VM",
                            "Undefined variable read (LOOK_WARN_UNDEF transition mode, returned null): " + gname);
                        it = globals_.emplace(gname, Value()).first;
                    }
                    slot = &it->second;
                    (*gcache)[ni] = slot;
                }
                slot->append_in_place(R(ins.a));
                break;
            }

            // ── Arithmetic — use Value's operators ───────────────────────────
            case OpCode::ADD: {
                // INT+INT fast-path: skip Value::operator+ machinery (arith_check×2,
                // to_int×2, FLOAT-checks) for the common case — measured ~8.5ns/add,
                // ~28× a native add. Any non-INT / overflow falls through to operator+
                // so FLOAT promotion and non-numeric fail-loud semantics are unchanged.
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_add_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b + c;
                break;
            }
            case OpCode::SUB: {
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_sub_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b - c;
                break;
            }
            case OpCode::MUL: {
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_mul_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b * c;
                break;
            }
            case OpCode::DIV:    R(ins.a) = R(ins.b) / R(ins.c);        break;
            case OpCode::MOD:    R(ins.a) = R(ins.b) % R(ins.c);        break;
            case OpCode::POW:    R(ins.a) = R(ins.b).pow(R(ins.c));     break;
            case OpCode::UNM: {
                const Value& b = R(ins.b);
                if (b.type()==Value::FLOAT) {
                    R(ins.a) = Value(-b.as_float());
                } else if (b.type()==Value::INT) {
                    int64_t v = b.as_int();
                    // -INT64_MIN does not fit in int64: overflow error (it used to silently become a double).
                    if (v == INT64_MIN) look::look_int_overflow(0, "-", v);
                    R(ins.a) = Value(-v);
                } else {
                    // Any other operand follows the binary operators' rules (0 - x): -"5" = -5, -"2.5" = -2.5,
                    // -true = -1, and -null / -"abc" are errors. It used to read everything with to_int:
                    // -"2.5" was -2, and -null and -"abc" were a silent 0.
                    R(ins.a) = Value((int64_t)0) - b;
                }
                break;
            }
            case OpCode::BAND:   R(ins.a) = R(ins.b).bitwise_and(R(ins.c)); break;
            case OpCode::BOR:    R(ins.a) = R(ins.b).bitwise_or(R(ins.c));  break;
            case OpCode::BXOR:   R(ins.a) = R(ins.b).bitwise_xor(R(ins.c)); break;
            case OpCode::BNOT:   R(ins.a) = R(ins.b).bitwise_not();         break;
            case OpCode::SHL:    R(ins.a) = R(ins.b).shift_left(R(ins.c));  break;
            case OpCode::SHR:    R(ins.a) = R(ins.b).shift_right(R(ins.c)); break;
            case OpCode::CONCAT:
                // B7: if dst==left operand, append in place to the accumulator (amortized O(1)).
                // `.=` and `$s=$s.x` produce this path; the general `a.b` (a!=b) takes the old path.
                if (ins.a == ins.b) R(ins.a).append_in_place(R(ins.c));
                else                R(ins.a) = R(ins.b).concat(R(ins.c));
                break;

            // ── Comparison ────────────────────────────────────────────────────
            case OpCode::EQ:  R(ins.a) = Value(R(ins.b) == R(ins.c)); break;
            case OpCode::NEQ: R(ins.a) = Value(!(R(ins.b) == R(ins.c))); break;
            case OpCode::LT:  R(ins.a) = Value(R(ins.b) <  R(ins.c)); break;
            case OpCode::GT:  R(ins.a) = Value(R(ins.b) >  R(ins.c)); break;
            case OpCode::LTE: R(ins.a) = Value(R(ins.b) <= R(ins.c)); break;
            case OpCode::GTE: R(ins.a) = Value(R(ins.b) >= R(ins.c)); break;
            case OpCode::CMP3: R(ins.a) = Value(R(ins.b).spaceship(R(ins.c))); break;
            case OpCode::NOT: R(ins.a) = Value(!R(ins.b).is_truthy()); break;
            case OpCode::COALESCE:
                R(ins.a) = (R(ins.b).type() != Value::NONE) ? R(ins.b) : R(ins.c);
                break;

            // ── Array ─────────────────────────────────────────────────────────
            case OpCode::NEW_ARRAY: {
                auto v = std::make_shared<ValueArray>();
                v->reserve(ins.b);
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::NEW_ASSOC: {
                // The "__assoc__" sentinel is a compile-time constant; allocating a fresh
                // make_shared<string> for it on every assoc creation was pure waste (5M
                // allocations in the object_create benchmark, and one per JSON response on
                // the web path). Share one immutable instance — push_back is a shared_ptr
                // refcount bump, not an allocation. Thread-safe: magic-static init + atomic
                // refcount, and the sentinel is only ever compared, never mutated.
                static const Value assoc_sentinel = Value::assoc_marker();
                auto v = std::make_shared<ValueArray>();
                if (ins.b > 0) v->reserve(ins.b);   // compiler-known final size (sentinel + 2/pair)
                v->push_back(assoc_sentinel);
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::ARRAY_PUSH:
                if (R(ins.a).type() == Value::ARRAY)
                    R(ins.a).as_array()->push_back(R(ins.b));
                break;
            case OpCode::ARRAY_GET:
                R(ins.a) = array_get(R(ins.b), R(ins.c));
                break;
            case OpCode::ARRAY_SET:
                array_set(R(ins.a), R(ins.b), R(ins.c));
                break;
            case OpCode::ASSOC_APPEND: {
                // Literal construction with compiler-proven-distinct string keys: skip the
                // O(n) dedup scan array_set runs on every insert (that scan made building an
                // n-field literal O(n^2)). The data structure is unchanged — still the
                // ["__assoc__", k, v, ...] sentinel array with O(n) lookup; only the
                // redundant build-time scan is elided.
                auto& vec = *R(ins.a).as_array();
                vec.push_back(R(ins.b));
                vec.push_back(R(ins.c));
                break;
            }
            case OpCode::CHECK_DEFINED: {
                // A bare name was read as a value and LOAD_GLOBAL gave null: is the name really
                // undefined (not a const with a null value) → an "Undefined variable" error.
                if (R(ins.a).type() == Value::NONE) {
                    const std::string& nm = CONST(ins.bx()).str_ref();
                    if (globals_.find(nm) == globals_.end())
                        throw LookVmError("Undefined variable: " + nm);
                }
                break;
            }
            case OpCode::ARRAY_LEN: {
                const Value& arr = R(ins.b);
                int len = 0;
                if (arr.type() == Value::ARRAY) {
                    auto& vec = *arr.as_array();
                    // Sentinel-first: ["__assoc__", k, v, ...] → (size-1)/2 pairs
                    if (!vec.empty() && vec[0].is_assoc_marker()) {
                        size_t data_start = 1;
                        if (vec.size()>2 && vec[1].as_string()=="__struct__") data_start=3;
                        len = (int)(vec.size()-data_start)/2;
                    } else {
                        len = (int)vec.size();
                    }
                }
                R(ins.a) = Value(len);
                break;
            }

            // ── Struct — Value::STRUCT, slots [name, definition, field0, field1, ...] ─────────
            case OpCode::NEW_STRUCT: {
                uint32_t ni = ins.bx();
                const std::string& sname = CONST(ni).str_ref();
                // Literal field-name block: NOP(n) + n×LOAD_CONST_W (produced by compile_struct_lit).
                const bool has_names = frame.ip < (int)proto->code.size()
                                    && proto->code[frame.ip].op == OpCode::NOP;
                const int  fc = has_names ? proto->code[frame.ip].a : 0;
                // Definition: the hidden global "__sdef:Name" written at the declaration's own position
                // (compile_struct_decl), triples [field, default, type]. The instance carries the definition in slot
                // 1; the literal values are then put in place with SET_FIELD.
                auto dit = globals_.find("__sdef:" + sname);
                if (dit == globals_.end() || dit->second.type() != Value::ARRAY)
                    throw LookVmError("Unknown struct '" + sname + "'");   // the tests grep this text
                const auto& def = *dit->second.as_array();
                for (int k = 0; k < fc; ++k) {
                    const auto& li = proto->code[frame.ip + 1 + k];
                    const std::string& fname = CONST(li.bx()).str_ref();
                    if (look::struct_field_index(def, fname) < 0)   // the tests grep this text
                        throw LookVmError("Unknown field '" + fname + "' in struct '" + sname + "'");
                }
                R(ins.a) = vm_struct([&] {
                    return look::struct_new(sname, dit->second,
                        [&](const std::string& t) { return globals_.count("__sdef:" + t) != 0; });
                });
                if (has_names) frame.ip += 1 + fc;   // skip the field-name block (execution)
                break;
            }
            case OpCode::GET_FIELD: {
                const Value& obj = R(ins.b);
                const std::string& fname = CONST(ins.c).str_ref();   // copy-free (as_string used to copy)
                if (obj.type() == Value::STRUCT) {
                    // Hot path: try whichever slot this instruction last found in whichever definition.
                    auto& sv = *obj.vec_ptr();
                    FieldCache& fc = field_cache()[(size_t)(frame.ip - 1)];
                    const void* def = sv[1].vec_ptr();
                    if (fc.def.get() != def) {
                        int i = look::struct_field_index(look::struct_def(sv), fname);
                        if (i < 0) vm_struct([&] { look::struct_no_field(look::struct_name(sv), fname); });
                        fc.def = sv[1].as_array(); fc.slot = (int)look::STRUCT_SLOT0 + i;
                    }
                    R(ins.a) = sv[(size_t)fc.slot];
                    break;
                }
                R(ins.a) = get_field(obj, fname);
                break;
            }
            case OpCode::SET_FIELD: {
                Value& obj = R(ins.a);
                const std::string& fname = CONST(ins.b).str_ref();
                if (obj.type() == Value::STRUCT) {
                    auto& sv = *obj.vec_ptr();
                    FieldCache& fc = field_cache()[(size_t)(frame.ip - 1)];
                    const void* def = sv[1].vec_ptr();
                    if (fc.def.get() != def) {
                        int i = look::struct_field_index(look::struct_def(sv), fname);
                        if (i < 0) vm_struct([&] { look::struct_no_field(look::struct_name(sv), fname); });
                        fc.def = sv[1].as_array(); fc.slot = (int)look::STRUCT_SLOT0 + i;
                    }
                    vm_struct([&] { look::struct_set(sv, fc.slot - (int)look::STRUCT_SLOT0, R(ins.c)); });
                    break;
                }
                set_field(obj, fname, R(ins.c));
                break;
            }
            case OpCode::SET_PATH: {
                // a = value, b = first register of the keys, c = number of keys; the next NOP names the root.
                Value* root = path_root(proto->code[frame.ip++]);
                // The value may carry the same store as the target ($a[0] = $a): keep its copy first.
                Value v = R(ins.a);
                set_path(*root, &R(ins.b), ins.c, v);
                break;
            }
            case OpCode::PUSH_PATH: case OpCode::POP_PATH: {
                // push/pop change the VARIABLE that is the first argument: the target is made single-
                // owner in the root's own place (copy-on-write), then grows/shrinks in place.
                const int ins_ip = frame.ip - 1;
                Value* root = path_root(proto->code[frame.ip++]);
                const bool is_push = ins.op == OpCode::PUSH_PATH;
                const Instruction* vh = is_push ? &proto->code[frame.ip++] : nullptr;
                Value pv = is_push ? R(vh->a) : Value();
                if (is_push && vh->b) {   // `$a = array::push($a, v)` — warn if there is no `use array` (then an error)
                    static const int push_ix = builtin_index("array::push");
                    if (push_ix >= 0 && !(*shared_.builtins)[push_ix])
                        warn_in_place_without_use_array(proto, ins_ip, "array::push()");
                }
                R(ins.a) = vm_struct([&] {
                    Value& target = look::value_path_slot(*root, &R(ins.b), ins.c,
                        [&](Value& c, const Value& k, const Value* forced) { array_set(c, k, *forced); });
                    // `$a = array::push($a, v)` was compiled in place (compiler try_compile_push_assign): the same message.
                    if (is_push && vh->b && target.type() != Value::ARRAY) {
                        if (target.type() == Value::STRUCT)   // same as the array:: wrapper in extra_stdlib.cpp
                            throw std::runtime_error("array::push() does not take a struct ('"
                                + (*target.as_struct())[0].str_ref() + "'): a struct is not an array, use its fields");
                        throw std::runtime_error("array::push() requires array and value");
                    }
                    return is_push ? look::value_push(target, pv, vh->b ? "array::push()" : "push()")
                                   : look::value_pop(target);
                });
                break;
            }
            case OpCode::SHRINK_PATH: {
                // `$a = array::slice($a, 0, -1)` in place (compiler try_compile_push_assign): the last element
                // is dropped in the variable's own place (one copy if shared). Errors in the same order and
                // text as the call's (the array:: wrapper in extra_stdlib.cpp + the slice body).
                const int ins_ip = frame.ip - 1;
                Value* root = path_root(proto->code[frame.ip++]);
                {   // warn if there is no `use array` (as in PUSH_PATH; then an error)
                    static const int slice_ix = builtin_index("array::slice");
                    if (slice_ix >= 0 && !(*shared_.builtins)[slice_ix])
                        warn_in_place_without_use_array(proto, ins_ip, "array::slice()");
                }
                vm_struct([&] {
                    Value& t = *root;
                    if (t.type() == Value::STRUCT)
                        throw std::runtime_error("array::slice() does not take a struct ('"
                            + (*t.as_struct())[0].str_ref() + "'): a struct is not an array, use its fields");
                    look::value_list_only(t, "array::slice()");
                    if (t.type() != Value::ARRAY) throw std::runtime_error("array::slice() requires array");
                    if (t.vec_ptr()->empty()) return;   // [] → [] (same value as a new empty list)
                    t.detach();
                    t.vec_ptr()->pop_back();
                });
                break;
            }

            // ── Jump ──────────────────────────────────────────────────────────
            // Backward jump = a new turn of the loop → yield point (fiber.h preempt_point).
#define LOOK_VM_JUMP_TO(target) do { const int _t = (target); \
                if (_t < frame.ip) vm_safe_point(); frame.ip = _t; } while (0)
            case OpCode::JUMP:
                LOOK_VM_JUMP_TO(ins.bx());
                break;
            case OpCode::JUMP_IF_FALSE:
                if (!R(ins.a).is_truthy()) LOOK_VM_JUMP_TO(ins.bx());
                break;
            case OpCode::JUMP_IF_TRUE:
                if ( R(ins.a).is_truthy()) LOOK_VM_JUMP_TO(ins.bx());
                break;
            case OpCode::JUMP_IF_NULL:
                if (R(ins.a).type()==Value::NONE) LOOK_VM_JUMP_TO(ins.bx());
                break;
#undef LOOK_VM_JUMP_TO

            // ── CALL ──────────────────────────────────────────────────────────
            case OpCode::CALL: {
                // a=ret_reg, b=fn_reg, c=args_base; next NOP(argc)
                vm_safe_point();   // recursion can run long even without a loop (fiber.h)
                uint16_t argc = 0;
                uint32_t cname = 0;   // constant index of the callee's name + 1 (0 = no name)
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc  = proto->code[frame.ip].a;
                    cname = proto->code[frame.ip].bx();
                    ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN) {
                    // OLD BUG: the message did not name it — "Not callable
                    // (BYTECODE_FN expected)". Which name? tree-walk (REFERENCE) said
                    // "Undefined variable: <name>"; the engines diverged both in the message and in
                    // diagnosability (S3).
                    if (cname) {
                        const std::string& nm = CONST(cname - 1).str_ref();
                        if (fn_val.type() == Value::NONE)
                            throw LookVmError("Undefined variable: " + nm);
                        throw LookVmError("'" + nm + "' is not callable (function expected)");
                    }
                    throw LookVmError("Not callable (function expected)");
                }
                // A raw pointer: the register keeps the closure alive while the frame is set up (grow_regs
                // moves the Value, not the closure), and the frame stores a raw pointer anyway. The old
                // shared_ptr copy cost two atomic refcount operations on every call.
                Closure* cl = fn_val.bytecode_fn_ptr();
                if (cl->native) {   // host function: like a built-in call
                    auto hold = fn_val.as_bytecode_fn();   // the host call may re-enter the VM and reuse registers
                    std::vector<Value> nargs;
                    nargs.reserve(argc);
                    for (int i = 0; i < argc; ++i) nargs.push_back(R(ins.c + i));
                    R(ins.a) = hold->native(nargs);
                    goto call_dispatch;   // a callback may have re-entered the VM and grown the stack
                }
                auto* cp = cl->proto.get();
                check_argc(cp, argc);
                int new_base = (int)regs_.size();
                grow_regs((size_t)new_base + cp->reg_count);
                if (!cp->variadic) {
                    for (int i = 0; i < argc && i < cp->arity; ++i)
                        regs_[new_base + i] = R(ins.c + i);
                } else {
                    // Variadic: bind the fixed params, collect the rest into the rest ARRAY
                    // (same as call_closure — it was missing on the CALL path).
                    int vfixed = cp->arity - 1;
                    for (int i = 0; i < vfixed && i < argc; ++i)
                        regs_[new_base + i] = R(ins.c + i);
                    auto varr = std::make_shared<look::ValueArray>();
                    for (int i = vfixed; i < argc; ++i) varr->push_back(R(ins.c + i));
                    if (vfixed >= 0 && cp->arity > 0)
                        regs_[new_base + vfixed] = look::Value(varr);
                }
                // MAX_CALL_DEPTH must be enforced on the bytecode CALL path too (bug 57):
                // it used to be checked only by call_closure (host→VM); direct
                // LOOK recursion (the CALL opcode) was UNBOUNDED → runaway/deep
                // recursion went on until memory ran out instead of a graceful error
                // (OOM DoS) and diverged from tree-walk's limit of 256.
                if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
                    stack_overflow_calls();
                int ret_abs = base + ins.a;
                call_stack_.push_back({cl->proto.get(), cl, 0, new_base, ret_abs, (int)argc});
                goto call_dispatch; // switch to the callee frame
            }

            case OpCode::CALL_BUILTIN: {
                // Builtin index: the low half is in ins.b, the high half in the b field of the following
                // NOP hint (a=argc) — Instruction::builtin_index. When it was 8-bit, the 256th entry was SILENTLY
                // truncated and called the wrong builtin (index 256 → 0 → print).
                uint16_t argc2 = 1;
                uint32_t bidx = ins.b;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc2 = proto->code[frame.ip].a;
                    bidx = Instruction::builtin_index(ins, proto->code[frame.ip]);
                    ++frame.ip;
                }
                if (!shared_.builtins || bidx >= shared_.builtins->size())
                    throw LookVmError("Unknown built-in: " + std::to_string(bidx));
                std::vector<Value> args;
                args.reserve(argc2);
                for (int i = 0; i < argc2; ++i) args.push_back(R(ins.c + i));
                // If the module was not 'use'd, the entry in the table is an EMPTY std::function and
                // calling it throws C++ std::bad_function_call.
                // OLD BUG: this exception reached the user as is:
                //   print(string::len("abc"))   ->  "Runtime Error: bad_function_call"
                // It names neither the module nor the function, and it is a C++ internal term at that. Meanwhile
                // tree-walk (REFERENCE) gave the right message: "Module 'string' not
                // loaded." — since forgetting 'use' is the most common mistake in LOOK,
                // the default engine gave the worst message. The same text is used (S3).
                {
                    const auto& bfn = (*shared_.builtins)[bidx];
                    if (!bfn) {
                        const auto& names = builtin_names();
                        std::string nm = bidx < names.size() ? names[bidx] : std::string();
                        size_t sep = nm.find("::");
                        if (sep != std::string::npos)
                            throw LookVmError("Module '" + nm.substr(0, sep) + "' not loaded.");
                        throw LookVmError("Built-in '" + (nm.empty() ? std::to_string(bidx) : nm) +
                                          "' unavailable (not linked)");
                    }
                    // parallel() task (end of script): a built-in that leaves a trace is an effect point.
                    bool in_effect = false;
                    if (task_) {
                        const auto& lt = builtin_leaves_trace();
                        in_effect = task_point(task_, bidx >= lt.size() || lt[bidx]);
                    }
                    TaskEffectEnd eff_end{in_effect ? task_ : nullptr};
                    R(ins.a) = bfn(args);
                }
                // Re-entrancy safety: a builtin (e.g. array::map) may have re-entered the VM through a callback
                // via call_closure and reallocated call_stack_
                // → the outer frame/proto references become invalid. Rebind by going back to
                // call_dispatch (frame.ip is already past the builtin+NOP).
                goto call_dispatch;
            }

            case OpCode::RETURN: {
                Value ret = R(ins.a);
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = std::move(ret);
                    goto call_dispatch; // return to the caller frame
                } else {
                    return ret;
                }
            }
            case OpCode::RETURN_NULL: {
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = Value();
                    goto call_dispatch; // return to the caller frame
                } else {
                    return Value();
                }
            }
            case OpCode::TAIL_CALL: {
                // The same CALL logic — reuse the frame (tail call opt)
                // For now behave like a normal CALL — not optimized
                uint16_t argc3 = 0;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc3 = proto->code[frame.ip].a; ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN)
                    throw LookVmError("TAIL_CALL: BYTECODE_FN expected");
                Closure* cl = fn_val.bytecode_fn_ptr();   // raw: see CALL
                if (cl->native) {   // host function: the result goes to R(a), the RETURN that follows returns it
                    auto hold = fn_val.as_bytecode_fn();
                    std::vector<Value> nargs;
                    nargs.reserve(argc3);
                    for (int i = 0; i < argc3; ++i) nargs.push_back(R(ins.c + i));
                    R(ins.a) = hold->native(nargs);
                    goto call_dispatch;
                }
                auto* cp = cl->proto.get();
                check_argc(cp, argc3);
                int new_base = (int)regs_.size();
                grow_regs((size_t)new_base + cp->reg_count);
                if (!cp->variadic) {
                    for (int i = 0; i < argc3 && i < cp->arity; ++i) regs_[new_base+i] = R(ins.c+i);
                } else {
                    int vfixed = cp->arity - 1;
                    for (int i = 0; i < vfixed && i < argc3; ++i) regs_[new_base+i] = R(ins.c+i);
                    auto varr = std::make_shared<look::ValueArray>();
                    for (int i = vfixed; i < argc3; ++i) varr->push_back(R(ins.c+i));
                    if (vfixed >= 0 && cp->arity > 0) regs_[new_base+vfixed] = look::Value(varr);
                }
                // MAX_CALL_DEPTH — CALL_METHOD/second CALL path (bug 57, see above)
                if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
                    stack_overflow_calls();
                call_stack_.push_back({cp, cl, 0, new_base, base+ins.a, (int)argc3});
                goto call_dispatch; // switch to the callee frame
            }

            // ── Closure ───────────────────────────────────────────────────────
            case OpCode::MAKE_CLOSURE: {
                // 32-bit index: b = low, c = high half — Instruction::bx_lohi (v1: only b → the 256th closure wrapped to the 0th).
                auto& nested_proto = proto->nested[ins.bx_lohi()];
                auto cl = std::make_shared<Closure>(nested_proto);
                // Read the capture hints: LOAD_CAPTURE(0, cr) pattern
                while (frame.ip < (int)proto->code.size()) {
                    const auto& h = proto->code[frame.ip];
                    if (h.op != OpCode::LOAD_CAPTURE || h.a != 0) break;
                    cl->captures.push_back(R(h.b));
                    ++frame.ip;
                }
                R(ins.a) = Value(cl);
                break;
            }
            case OpCode::LOAD_CAPTURE: {
                if (!frame.closure || ins.b >= frame.closure->captures.size())
                    throw LookVmError("Capture index out of range: " + std::to_string(ins.b));
                R(ins.a) = frame.closure->captures[ins.b];
                break;
            }

            // ── Foreach ───────────────────────────────────────────────────────
            case OpCode::FOR_PREP:
                // r_iter = array copy, r_iter+1 = index 0
                // foreach over a struct walks the fields (name => value) → a map view.
                R(ins.a)   = R(ins.b).type() == Value::STRUCT ? look::struct_to_map(R(ins.b)) : R(ins.b);
                R(ins.a+1) = Value(0);
                break;
            case OpCode::FOR_STEP: {
                const Value& arr = R(ins.a);
                int idx = R(ins.a+1).as_int();
                if (arr.type() != Value::ARRAY) { frame.ip=ins.bx(); break; }
                auto& vec = *arr.as_array();
                // Sentinel-first: ["__assoc__", k0, v0, k1, v1, ...]
                bool is_assoc = !vec.empty() && vec[0].is_assoc_marker();
                if (is_assoc) {
                    // Skip __assoc__ sentinel + optional __struct__ tag
                    int data_start = 1;
                    if (vec.size()>2 && vec[1].type()==Value::STRING && vec[1].as_string()=="__struct__")
                        data_start = 3;
                    int pi = data_start + idx * 2;
                    if (pi + 1 >= (int)vec.size()) {
                        frame.ip = ins.bx();
                    } else {
                        R(ins.a+3) = vec[pi];     // key
                        R(ins.a+2) = vec[pi+1];   // val
                        R(ins.a+1) = Value(idx+1);
                    }
                } else {
                    if (idx >= (int)vec.size()) {
                        frame.ip = ins.bx();
                    } else {
                        R(ins.a+2) = vec[idx];    // val
                        R(ins.a+3) = Value(idx);  // key
                        R(ins.a+1) = Value(idx+1);
                    }
                }
                break;
            }

            // ── Try/Catch ─────────────────────────────────────────────────────
            case OpCode::TRY_PUSH: {
                int catch_ip = ins.bx();
                try_stack_.push_back({catch_ip, (int)call_stack_.size(), base});
                break;
            }
            case OpCode::TRY_POP:
                if (!try_stack_.empty()) try_stack_.pop_back();
                break;
            case OpCode::THROW: {
                current_exception_ = R(ins.a);
                // If there is no handler belonging to this run(): propagate the value across the C++ boundary.
                // (Jumping to the outer run()'s handler corrupts the call stack and skips the catch of the
                //  C++ layer in between — e.g. the db::transaction ROLLBACK.)
                if (try_stack_.size() <= try_floor_)
                    throw LookVmThrow(current_exception_);
                auto entry = try_stack_.back(); try_stack_.pop_back();
                while ((int)call_stack_.size() > entry.frame_depth) {
                    regs_.resize(call_stack_.back().base);
                    call_stack_.pop_back();
                }
                call_stack_.back().ip = entry.catch_ip;
                // REBIND frame/proto/base to the frame where the catch is (same as RETURN).
                // A bare `break` continued the inner loop with a stale `proto` + a dangling `frame` reference
                // (a throw crossing a function boundary → silent stop/UB; VM≠interpreter).
                goto call_dispatch;
            }
            case OpCode::LOAD_EXC:
                R(ins.a) = current_exception_;
                break;
            case OpCode::LOAD_ARGC:
                R(ins.a) = Value((int64_t)frame.argc);
                break;

            // ── LOOK-specific ─────────────────────────────────────────────────
            case OpCode::PARALLEL_CALL: {
                if (R(ins.a).type() != Value::BYTECODE_FN)
                    throw LookVmError("parallel(): BYTECODE_FN expected");
                const bool in_effect = task_ && task_point(task_, true);   // a task from inside a task
                TaskEffectEnd eff_end{in_effect ? task_ : nullptr};
                task_acquire(); // THROW mode: throws if LOOK_PARALLEL_LIMIT reached
                auto src_cl = R(ins.a).as_bytecode_fn();
                // Captured values are cloned when carried to the thread: so the closure and
                // the caller do not share the same array/map storage (sharing = a race).
                std::shared_ptr<Closure> cl_copy = src_cl;
                {
                    // A captured closure (BYTECODE_FN) is cloned too: deep_clone clones it
                    // transitively (bc_fn_cloner hook, registered below).
#ifdef LOOK_NO_TRANSITIVE_CLONE
                    constexpr bool CLONE_CLOSURES = false; // TSan positive control: the old (racy) state
#else
                    constexpr bool CLONE_CLOSURES = true;
#endif
                    // R-02: capture isolation — instead of an ALLOW LIST (clone only closures)
                    // a DENY LIST (fail-safe). Every capture is cloned EXCEPT the handles whose sharing is INTENTIONAL
                    // (CHANNEL/WS/SSE — cross-thread communication).
                    // The old state shared a plain ARRAY capture shallowly → parallel tasks mutated the same
                    // vector and RACED (t1b VM, TSan). deep_clone_impl
                    // handles every type correctly (array recursive, handle *this). When a new Value type is
                    // added it starts automatically on the safe side. (CLONE_CLOSURES=false =
                    // LOOK_NO_TRANSITIVE_CLONE positive control: the old racy state.)
                    auto must_clone = [](Value::Type t) {
                        return t != Value::CHANNEL && t != Value::WEBSOCKET && t != Value::SSE_CONN;
                    };
                    bool needs = false;
                    for (size_t i = 0; i < src_cl->captures.size(); ++i) {
                        if (CLONE_CLOSURES && must_clone(src_cl->captures[i].type())) { needs = true; break; }
                    }
                    if (needs) {
                        cl_copy = std::make_shared<Closure>(src_cl->proto);
                        cl_copy->captures = src_cl->captures;   // shallow (shared_ptr shares)
                        for (size_t i = 0; i < cl_copy->captures.size(); ++i) {
                            if (CLONE_CLOSURES && must_clone(cl_copy->captures[i].type()))
                                cl_copy->captures[i] = cl_copy->captures[i].clone_for_thread();
                        }
                    }
                }
                SharedState sh = shared_;
                std::unordered_map<std::string, Value> g_copy = globals_;
                // builtins and routes point to local pointers — deep copy for the parallel task
                std::vector<BuiltinFn> builtins_copy = sh.builtins ? *sh.builtins : std::vector<BuiltinFn>{};
                sh.builtins = nullptr;
                sh.routes   = nullptr; // a parallel task does not call dispatch_routes
                // The end-of-script registration BEFORE the thread STARTS: if the script ends right away the task is counted too.
                TaskExit* texit = task_exit_new();
                try {
                std::thread([cl_copy, sh, g_copy, builtins_copy = std::move(builtins_copy), texit]() mutable {
                    TaskGuard _guard; // task_release() on scope exit
                    // Returning the DB connection: if db::query is called
                    // inside the task, get_conn takes a lazy conn for this thread;
                    // if it is not released it leaks PERMANENTLY from the pool (after a few requests
                    // the pool is exhausted, the endpoint hangs forever).
                    struct DbGuard { ~DbGuard() { look::release_thread_connections(); } } _db;
                    std::ostringstream sink;
                    VM tvm(sh, sink);
                    tvm.set_globals(std::move(g_copy));
                    tvm.set_builtins(&builtins_copy);
                    tvm.set_task(texit);
                    task_exit_enter(texit);   // this thread's task registration (preempt check)
                    struct ExitDone { TaskExit* t; ~ExitDone() { task_exit_done(t); } } _done{texit};
                    try { tvm.call_closure(*cl_copy, {}); }
                    catch (const std::exception& e) {
                        Logger::instance().log(LogLevel::LOG_ERROR, "VM",
                            std::string("parallel() panic: ") + e.what());
                    } catch (...) {
                        Logger::instance().log(LogLevel::LOG_ERROR, "VM",
                            "parallel() panic: unknown error");
                    }
                }).detach();
                } catch (...) { task_exit_done(texit); throw; }   // the thread could not be started
                break;
            }
            // CHAN_* — TYPE CHECK REQUIRED: as_channel() does a static_pointer_cast without
            // looking at type_; if the argument is ARRAY/STRING/FN (the compiler produces send/receive/close/chan_size
            // without looking at the type) that object's memory is taken for a LookChannel → TYPE CONFUSION
            // → heap-buffer-overflow (ASan-proven). The old interpreter already
            // checked it; the VM did not (engine divergence, S3). e.g. send([1,2,3],42).
            case OpCode::CHAN_SEND: {
                if (R(ins.a).type() != Value::CHANNEL)
                    throw LookVmError("send(): argument is not a channel");
                const bool in_effect = task_ && task_point(task_, true);
                TaskEffectEnd eff_end{in_effect ? task_ : nullptr};
                R(ins.a).as_channel()->send_val(R(ins.b), in_effect ? &task_->phase : nullptr);
                break;
            }
            case OpCode::CHAN_RECV: {
                if (R(ins.b).type() != Value::CHANNEL)
                    throw LookVmError("receive(): argument is not a channel");
                const bool in_effect = task_ && task_point(task_, true);
                TaskEffectEnd eff_end{in_effect ? task_ : nullptr};
                R(ins.a) = R(ins.b).as_channel()->recv_val();
                break;
            }
            case OpCode::CHAN_CLOSE: {
                if (R(ins.a).type() != Value::CHANNEL)
                    throw LookVmError("close(): argument is not a channel");
                const bool in_effect = task_ && task_point(task_, true);
                TaskEffectEnd eff_end{in_effect ? task_ : nullptr};
                // close_chan: wakes the receivers AND the waiting senders (the sender gets "send on
                // closed channel"). It used to wake only the receivers → a send
                // waiting on a full channel stayed hung forever.
                R(ins.a).as_channel()->close_chan();
                break;
            }
            case OpCode::CHAN_SIZE: {
                if (R(ins.b).type() != Value::CHANNEL)
                    throw LookVmError("chan_size(): argument is not a channel");
                R(ins.a) = Value(R(ins.b).as_channel()->sz());
                break;
            }

            // ── Cast / Type ───────────────────────────────────────────────────
            case OpCode::TO_INT:   R(ins.a) = look::look_explicit_int(R(ins.b));   break;   // out-of-range numeric text → error
            case OpCode::TO_FLOAT: R(ins.a) = look::look_explicit_float(R(ins.b)); break;
            case OpCode::TO_STR:   R(ins.a) = Value(R(ins.b).to_string()); break;
            case OpCode::TO_BOOL:  R(ins.a) = Value(R(ins.b).is_truthy()); break;
            case OpCode::TYPE_OF: {
                static const char* tnames[] = {
                    "int","float","string","bool","function","array",
                    "channel","websocket","sse","function","null"
                };
                int ti = (int)R(ins.b).type();
                R(ins.a) = Value(std::string(ti < 11 ? tnames[ti] : "unknown"));
                break;
            }

            case OpCode::NOP:       break;
            case OpCode::BREAKPOINT:
                Logger::instance().log(LogLevel::LOG_DEBUG, "VM", "BREAKPOINT");
                break;

            default:
                throw LookVmError("Unknown opcode: " + std::to_string((int)ins.op));

#undef R
#undef CONST
            } // switch
        } // inner while
        }
        // ── C++ exception → LOOK try/catch bridge ─────────────────────────────
        // A runtime error thrown by operators/builtins (division by zero, type
        // error...) is routed to try_stack_:
        // control-flow exceptions (exit/route/stop) are let through,
        // the rest of the std::exception falls into the catch block with its message. If try_stack_ is empty it is
        // passed up (an error response on the web, a top-level error in the CLI-VM).
        catch (const RouteStopException&)   { throw; }
        catch (const RouteMatchedException&){ throw; }
        catch (const ExitException&)        { throw; }
        // A memory stop is not given to LOOK try/catch (look/mem_budget.h): if it were, a
        // `try { … } catch` loop would swallow the stop and keep growing. The line is recorded for the log.
        catch (const MemoryLimitError&)     { note_error_line_for(base, proto); throw; }
        catch (const LookVmThrow& t) {
            // A LOOK throw that crossed the C++ boundary (inner run() → builtin → here).
            // The VALUE is preserved (including typed values like error::new) — not e.what().
            if (try_stack_.size() <= try_floor_) { note_error_line_for(base, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = t.value;
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }
        catch (const LookRuntimeError& e) {
            // error::new() DOES NOT RETURN A VALUE — it throws LookRuntimeError(payload) directly.
            // Preserving the typed payload is MANDATORY: reducing it to the e.what() string makes the catch
            // variable a "[type, E_DB, message, ...]" string → error::is()
            // returns false, error::message() gives the whole object (silently wrong error
            // handling). Rule: has_value ? value : message.
            if (try_stack_.size() <= try_floor_) { note_error_line_for(base, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = e.has_value ? e.value : Value(e.message);
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }
        catch (const std::exception& e) {
            if (try_stack_.size() <= try_floor_) { note_error_line_for(base, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = Value(std::string(e.what()));
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }

        // Implicit return null
        int ret_reg = frame.ret_reg;
        regs_.resize(base);
        call_stack_.pop_back();
        if (call_stack_.empty() || ret_reg < 0) return Value();
        regs_[ret_reg] = Value();

    } // outer while
    return Value();
}

} // namespace look

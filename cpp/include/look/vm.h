#pragma once

#include "look/bytecode.h"
#include "look/runtime_host.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <sstream>

namespace look {

struct TaskExit;   // parallel_runtime.h

// ── VM runtime error ──────────────────────────────────────────────────────────

class LookVmError : public std::runtime_error {
public:
    int line;
    explicit LookVmError(const std::string& msg, int line = 0)
        : std::runtime_error(msg), line(line) {}
};

// Carries the VALUE of a LOOK `throw` as it crosses a C++ boundary (VM → CALL_BUILTIN →
// module native fn → bridge → inner VM run()). The inner run() cannot jump to a handler
// below its own try floor (that would run outer frames inside the inner call and corrupt
// the call stack, and the catch of the C++ layer in between — e.g. db::transaction's ROLLBACK — would never run).
// Instead this exception propagates; the outer VM dispatch catches it, stores the value in
// current_exception_ and routes it to its own catch handler.
class LookVmThrow : public std::runtime_error {
public:
    Value value;
    explicit LookVmThrow(Value v)
        : std::runtime_error(v.to_string()), value(std::move(v)) {}
};

// ── BuiltinFn ─────────────────────────────────────────────────────────────────

using BuiltinFn = std::function<Value(std::vector<Value>&)>;

// Wraps a host function into a LOOK function value (see Closure::native). For names a runner puts
// into globals: the test(), assert_eq(), assert::eq … helpers of `lk test`.
Value make_native_fn(const std::string& name, BuiltinFn fn);

// VM route entry with route-level middleware support
struct VmRoute {
    std::string          pattern;     // "GET:/path/{id}"
    Closure*             fn;          // route handler closure
    std::vector<Closure*> middlewares; // route-level middleware list (filled during setup)
};

// ── VM ────────────────────────────────────────────────────────────────────────

class VM {
public:
    struct SharedState {
        const std::unordered_map<std::string, Value>*            globals     = nullptr;
        const std::vector<VmRoute>*                              routes      = nullptr;
        const std::unordered_map<std::string,
              std::vector<StructFieldDef>>*                       struct_defs = nullptr;
        // builtins: set per request (via set_builtins)
        const std::vector<BuiltinFn>*                            builtins    = nullptr;
    };

    explicit VM(SharedState shared, std::ostream& output);

    void set_globals(std::unordered_map<std::string, Value> g);
    // Request isolation (web): called after set_globals. Arrays/closures in the setup globals
    // are copied on FIRST access within a request (LOAD_GLOBAL slow path).
    // The per-request copy of a setup closure that captures an array (otherwise c itself).
    void set_web_context(WebContext* ctx);
    void set_ws_connection(std::shared_ptr<WsConnection> ws);
    void set_sse_connection(std::shared_ptr<SseConnection> sse);
    // Per-request builtins — must be called on every request (module fns are bound to ctx)
    void set_builtins(const std::vector<BuiltinFn>* b) { shared_.builtins = b; }
    // VM of a parallel() task: "is there work left" points at script end (parallel_runtime.h).
    void set_task(TaskExit* t) { task_ = t; }

    // Returns the value of a top-level `return <expr>` (the REPL shows the last expression this way); null if none.
    Value execute(const CompiledProgram& prog);
    void  dispatch_routes(const std::string& method, const std::string& path);
    // Source line at the moment an (uncaught) error escapes — 0 = unknown.
    // So the caller can produce the "Runtime Error ... File/Line" format.
    int   last_error_line() const { return last_error_line_; }
    // File of the error: "" = main script, otherwise a path relative to the main script's directory (included file).
    const std::string& last_error_file() const { return last_error_file_; }
    // Call chain of the error (outermost to innermost; LookRuntimeError::format prints the innermost first).
    const std::vector<StackFrame>& last_error_stack() const { return last_error_stack_; }
    Value call_closure(const Closure& closure, std::vector<Value> args);
    const std::unordered_map<std::string, Value>& get_globals() const { return globals_; }

private:
    // Call stack limits (measured, 2026-11-09). A LOOK call uses the heap, NOT the C++ stack
    // (frame + registers, ~330 B/level). The old fixed 500 was far below Node's (~9k).
    // Now at most 10,000 frames and 8 MB of registers — beyond that a catchable "Stack overflow".
    // THE LIMIT IS PER REQUEST and concurrent requests add up: with 1M calls / 256 MB at fec55ef,
    // 12 simultaneous infinite recursions pushed the server to 2.3 GB (measured; on a 2 GB
    // server the kernel kills the whole process). At 10,000 / 8 MB, 32 concurrent requests stay under ~400 MB
    // (tests/concurrent_recursion_memory_test.sh). Deeper recursion could only come with a
    // process-wide memory cap. The 8 MB budget also keeps a many-register function (255 regs =
    // 8 KB/level) from growing 10,000 levels to 80 MB.
    static constexpr int    MAX_CALL_DEPTH   = 10000;
    static constexpr size_t MAX_STACK_VALUES = (8u * 1024 * 1024) / sizeof(Value);
    // Callback nesting (a LOOK function called from map/filter/sort…): each level takes
    // ~1.5 KB of C++ stack (run + call_closure + builtin). Measured: 5,000 works and 6,000 crashes on an 8 MB stack
    // (same for fibers and threads). 4,000 (Node ~2k) plus a real remaining-stack check underneath
    // (look/stack_guard.h): no crash with a small ulimit -s or a small fiber stack either.
    static constexpr int    MAX_NATIVE_DEPTH = 4000;
    int native_depth_ = 0;
    // Grow the register array: the budget check only runs when capacity is full (one comparison on the hot path);
    // growth is clamped to the budget → doubling never makes a temporary allocation that exceeds the budget.
    void grow_regs(size_t n) {
        if (n > regs_.capacity()) reserve_regs(n);
        regs_.resize(n);
    }
    void reserve_regs(size_t n);   // vm.cpp: "Stack overflow" when the budget is exceeded

    std::vector<Value> regs_;

    struct Frame {
        const FunctionProto* proto;
        const Closure*       closure;
        int                  ip;
        int                  base;
        int                  ret_reg;
        int                  argc = 0;   // number of args supplied at the call (default-param prologue)
    };
    std::vector<Frame> call_stack_;

    struct TryCatchEntry { int catch_ip; int frame_depth; int reg_base; };
    std::vector<TryCatchEntry> try_stack_;
    // Nested run() floor: this run() call may ONLY use the handlers in try_stack_[try_floor_..].
    // Those below belong to outer run() calls — instead of jumping there,
    // the error propagates across the C++ boundary as LookVmThrow (see LookVmThrow).
    size_t try_floor_ = 0;
    Value current_exception_;

    Value run();
    int   push_frame(const Closure* cl, int reg_count, int ret_reg);

    // Route matching
    bool route_match(const std::string& pattern, const std::string& path,
                     std::vector<Value>& params);

    // Value helpers — delegated to Value's own methods where possible
    bool        val_truthy(const Value& v);
    std::string val_to_str(const Value& v);
    Value       array_get(const Value& arr, const Value& key);
    void        array_set(Value& arr, const Value& key, const Value& val);
    // Location of an existing element (nullptr if absent) and copy-on-write assignment along a path.
    Value*      array_slot(Value& arr, const Value& key);
    void        set_path(Value& slot, const Value* keys, int n, const Value& val);
    Value       get_field(const Value& obj, const std::string& field);
    void        set_field(Value& obj, const std::string& field, const Value& val);

    TaskExit* task_ = nullptr;  // null when not a parallel() task — hooks are a single branch
    int last_error_line_ = 0;   // line of the uncaught error (proto->lines[ip-1])
    std::string last_error_file_;   // ...and its file (proto->file_at; "" = main script)
    // ...and the call chain. The old interpreter printed "Stack trace: at f (line N)"; it was
    // lost in the move to the VM. Built from call_stack_ when the error is first recorded
    // (innermost, frames not yet unwound) — only on the error path, no cost on the normal path.
    std::vector<StackFrame> last_error_stack_;
    void capture_error_stack();

    // Record the line of the running opcode as an error escapes. frame.ip already points at
    // the next instruction (ip++ during fetch) → the faulty instruction is at ip-1.
    // Error path: finds this run()'s frame (register base + proto) in call_stack_ and records its line —
    // the `frame` reference in run() may be invalid after a nested call (vm.cpp).
    void note_error_line_for(int base, const FunctionProto* proto);
    // For the SAME error the FIRST (innermost) record is kept: as an error leaves a callback chain (array::map → LOOK
    // function), every nested run() wrote its own call site; the outermost
    // won, so the report showed the line of the map call instead of the error's line (the old
    // interpreter got it right). The error passes through the layers as the same object via `throw;`: if its identity
    // (exception_ptr) is already recorded, do not write. Called only on the error path (from a catch) → no cost on
    // the normal path and no reset needed (the first, flagged version added a branch to every call_closure: +0.26%).
    std::exception_ptr noted_error_;
    template <class F, class P>
    void note_error_line(const F& frame, const P* proto) {
        std::exception_ptr cur = std::current_exception();
        if (cur && cur == noted_error_) return;
        int i = frame.ip - 1;
        if (proto && i >= 0 && i < (int)proto->lines.size() && proto->lines[i] > 0) {
            last_error_line_ = proto->lines[i];
            last_error_file_ = proto->file_at(i);
            noted_error_ = cur;
            // If an outer run() has a try the error will be caught there: do not build the chain for nothing (an error thrown
            // from a callback and caught outside measured +1.7% in a loop → 0).
            if (try_stack_.empty()) capture_error_stack();
            else last_error_stack_.clear();
        }
    }

    // Shared read-only state
    SharedState                              shared_;
    std::unordered_map<std::string, Value>   globals_;
    // A2 inline cache: LOAD_GLOBAL paid globals_.find (hash + probe) on every access —
    // ~2.77× in a global-heavy CPU loop (measured). A per-proto cache of Value* addressed by the
    // constant-pool index (ni): on the first hit we pin the address of the globals_ slot,
    // later accesses run at register speed. SAFE because: (1) the VM is per-request/per-run (NO thread
    // sharing — bytecode is shared, this cache belongs to the VM), (2) globals_ never erases
    // (no unset/erase → Value* cannot dangle), (3) an unordered_map rehash does NOT
    // invalidate element pointers (insert is safe). The only invalidation: set_globals → clear (globals_
    // changes wholesale). The ni→name mapping is proto-specific, so the key is the proto pointer.
    std::unordered_map<const FunctionProto*, std::vector<Value*>> global_cache_;
    // GET_FIELD/SET_FIELD per-instruction cache — the last seen struct definition and slot.
    // Keeps the definition ALIVE (not a raw pointer): when a struct is redeclared and the old definition is freed,
    // the new one can land at the same address, and the cache then returned the slot of the old layout →
    // silently the wrong field (tests/struct_redeclare_cache_test.lk). Ownership is only
    // copied on a miss; the hot path is still a single pointer comparison. The cache belongs to the VM (every request
    // and every parallel thread builds its own VM) → not shared between threads.
    struct FieldCache { std::shared_ptr<std::vector<Value>> def; int slot = 0; };
    std::unordered_map<const FunctionProto*, std::vector<FieldCache>> field_cache_;
    WebContext*                              web_ctx_ = nullptr;
    std::ostream&                            output_;
    std::shared_ptr<WsConnection>            ws_conn_;
    std::shared_ptr<SseConnection>           sse_conn_;
};

// ── VMApp ─────────────────────────────────────────────────────────────────────

struct VMApp {
    CompiledProgram                                       program;
    std::unordered_map<std::string, Value>                setup_globals;
    std::vector<std::pair<std::string, Closure*>>         routes;
    std::unordered_map<std::string, std::vector<StructFieldDef>> struct_defs;
    std::vector<BuiltinFn>                                builtins;
};

} // namespace look

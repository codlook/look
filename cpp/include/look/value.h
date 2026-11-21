#pragma once
// The language's value (Value) and the core types beside it: channel, module table, error types.
// The host class (RuntimeHost) is in look/runtime_host.h.

#include "look/ast.h"
#include "look/format.h"   // look_format_double — the language's single double formatter
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace look { struct WebContext; }

namespace look {

// ── Forward declarations for Value members ────────────────────────────────────
struct LookChannel;    // defined after Value (queue<Value> needs complete type)
class Fiber;           // fiber.h — fibers waiting on a channel (LookChannel::FiberWaiter)
class FiberScheduler;
struct WsConnection;   // defined in websocket.h
struct SseConnection;  // defined in sse.h
struct Closure;        // defined in bytecode.h — BYTECODE_FN type

// Integer overflow error (value.cpp) — arithmetic, unary minus, abs and ** give the same message.
[[noreturn]] void look_int_overflow(int64_t a, const char* op, int64_t b);
class Value;
// Explicit conversion int() / float() (TO_INT / TO_FLOAT): loose like to_int/to_float ("abc" → 0), but an error if NUMERIC
// text is out of range (Go strconv ErrRange) — it used to be int("99999999999999999999") → 0 silently.
Value look_explicit_int(const Value& v);
Value look_explicit_float(const Value& v);

// Nesting depth of values (array/struct inside array/struct). Recursive operations that WALK the value
// (to-string conversion, ==, cloning) raise a catchable error when this limit is exceeded — like Python's
// RecursionError (PHP json 512, .NET 64). OLD: there was no limit; measured — the C++ stack overflowed at
// ~40–80k depth in a normal build and ~3–7k under ASan, and the WHOLE PROCESS crashed. 1000 is below the worst case
// (240 nested callbacks, ASan). RELEASING a value is unlimited (ValueArray).
constexpr int LOOK_MAX_NESTING = 1000;
[[noreturn]] void look_nesting_error();
// Set up only in array/struct branches (the scalar hot path is untouched).
struct NestingGuard {
    // fiber-ok: these operations never suspend (no user code is called, no I/O) — the counter rises and falls
    // within the same synchronous walk and is back to 0 when the walk ends.
    static int& depth() { thread_local int d = 0; return d; }
    NestingGuard() { if (++depth() > LOOK_MAX_NESTING) { --depth(); look_nesting_error(); } }
    ~NestingGuard() { --depth(); }
    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
};

// ── Value ─────────────────────────────────────────────────────────────────────

// Constructors of array/struct storage — ValueArray is defined after Value; the class body uses them.
inline std::shared_ptr<std::vector<Value>> look_new_array();
inline std::shared_ptr<std::vector<Value>> look_new_array(const std::vector<Value>& copy);

// Text that becomes a value does not carry spare capacity. OLD: text built by appending (concatenation, "a{$x}b",
// JSON output) doubled its capacity as it grew → a 1 MiB text took 2 MiB (measured:
// the request memory budget filled at half the real need). If there is more than 4 KiB and 25% slack
// it is shrunk to the exact size. Accumulation (`.=`, `$s = $s . x`) grows in its own buffer in append_in_place
// (amortized) — it does not go through this path.
inline std::string&& fit_string(std::string& s) {
    const size_t slack = s.capacity() - s.size();
    if (slack > 4096 && slack > s.size() / 4) s.shrink_to_fit();
    return std::move(s);
}

class Value {
public:
    // STRUCT was appended at the end → the numbers of existing types do not change. Its storage is the same as ARRAY
    // (shared_ptr<vector<Value>>) but its layout is different and FIXED:
    //   [0] = struct name (STRING)   [1] = definition (ARRAY, triples [field, default, type])
    //   [2 + i] = value of the i-th field (declaration order)
    // Being a separate type, no code that expects an array (array::, indexing, foreach) mistakes it for
    // an ARRAY; field access is an index, not a name lookup. Rules: look/struct_types.h.
    // The function value is a single kind: BYTECODE_FN (a VM closure). The FUNCTION kind that existed
    // (the tree-walking interpreter's closure) was removed together with the interpreter.
    enum Type { INT, FLOAT, STRING, BOOL, ARRAY, CHANNEL, WEBSOCKET, SSE_CONN, BYTECODE_FN, NONE, STRUCT };
    static Value make_struct(std::shared_ptr<std::vector<Value>> slots) {
        Value v(std::move(slots)); v.type_ = STRUCT; return v;
    }
    // Slot vector of a STRUCT (the same pointer as as_array; separate so the name shows the intent).
    std::shared_ptr<std::vector<Value>> as_struct() const { return std::static_pointer_cast<std::vector<Value>>(ptr_val); }
    // Non-owning access for the hot path: no shared_ptr copy (atomic counter increment/decrement).
    // Valid only while this Value is alive.
    std::vector<Value>* vec_ptr() const { return static_cast<std::vector<Value>*>(ptr_val.get()); }
    // Copy-on-write: if the array/struct storage is shared with another Value, this Value
    // takes its own (top-level) copy; if it is the sole owner it does nothing. Elements stay
    // shared — they are detached by the same rule at the moment they are written.
    void detach() {
        if ((type_ == ARRAY || type_ == STRUCT) && ptr_val && ptr_val.use_count() > 1)
            ptr_val = look_new_array(*vec_ptr());
    }

    // B5: scalars live in a union (8 bytes), STRING is behind a pointer → sizeof 80→32,
    // no string ctor/dtor on scalar copy. String literals are shared from the constant pool
    // via LOAD_CONST (refcount bump, no copy) → RAM + speed.
    Value()                                         : type_(NONE),     int_val(0)   {}
    explicit Value(int64_t i)                       : type_(INT),      int_val(i)   {}
    explicit Value(int i)                           : type_(INT),      int_val((int64_t)i) {}
    explicit Value(double d)                        : type_(FLOAT),    float_val(d) {}
    explicit Value(bool b)                          : type_(BOOL),     bool_val(b)  {}
    explicit Value(const std::string& s)            : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(s)) {}
    explicit Value(std::string&& s)                 : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(fit_string(s))) {}
    explicit Value(std::shared_ptr<std::vector<Value>> a) : type_(ARRAY), int_val(0), ptr_val(std::move(a)) {}
    explicit Value(std::shared_ptr<LookChannel>        c) : type_(CHANNEL),   int_val(0), ptr_val(std::move(c)) {}
    explicit Value(std::shared_ptr<WsConnection>       w) : type_(WEBSOCKET), int_val(0), ptr_val(std::move(w)) {}
    explicit Value(std::shared_ptr<SseConnection>      s) : type_(SSE_CONN),  int_val(0), ptr_val(std::move(s)) {}
    explicit Value(std::shared_ptr<Closure>            c) : type_(BYTECODE_FN), int_val(0), ptr_val(std::move(c)) {}

    Type        type()      const { return type_; }
    int64_t     as_int()    const { return int_val; }
    double      as_float()  const { return float_val; }
    std::string as_string() const {
        return (type_ == STRING && ptr_val)
            ? *static_cast<const std::string*>(ptr_val.get()) : std::string();
    }
    bool        as_bool()   const { return bool_val; }
    // Typed access from the single shared_ptr<void> — type_ guarantees which type is active.
    std::shared_ptr<std::vector<Value>>  as_array()    const { return std::static_pointer_cast<std::vector<Value>>(ptr_val); }
    std::shared_ptr<LookChannel>         as_channel()    const { return std::static_pointer_cast<LookChannel>(ptr_val); }
    std::shared_ptr<WsConnection>        as_websocket()  const { return std::static_pointer_cast<WsConnection>(ptr_val); }
    std::shared_ptr<SseConnection>       as_sse()        const { return std::static_pointer_cast<SseConnection>(ptr_val); }
    std::shared_ptr<Closure>             as_bytecode_fn()const { return std::static_pointer_cast<Closure>(ptr_val); }
    // Without the shared_ptr copy (two atomic refcount operations): for the VM's CALL, where the value
    // in the register keeps the closure alive for the whole call setup.
    Closure*                             bytecode_fn_ptr() const { return static_cast<Closure*>(ptr_val.get()); }

    std::string to_string() const;
    double      to_float()  const;
    int64_t     to_int()    const;
    bool        is_truthy() const;

    // Recursive deep copy for ARRAY; safe against cyclic references (broken with a visited set)
    Value deep_clone() const {
        std::unordered_set<const void*> visited;
        return deep_clone_impl(visited);
    }

    // Transitive deep-clone hook for BYTECODE_FN (Closure). value.h CANNOT SEE Closure
    // (bytecode.h includes it later) → the vm layer registers a cloner. If none is registered,
    // the old behavior (shallow). 58 parallel: when a closure is captured and deep_clone copies it
    // shallowly, the cells inside the closure stay SHARED with the parent → data race (np1). The cloner
    // clones the closure and recursively clones its cell captures + inner closures.
    using BcFnCloner = Value(*)(const Value&, std::unordered_set<const void*>&);
    static BcFnCloner& bc_fn_cloner() { static BcFnCloner h = nullptr; return h; }
    // Deep clone with a shared visited set for hook recursion (deep_clone_impl is private).
    Value deep_clone_tracked(std::unordered_set<const void*>& visited) const { return deep_clone_impl(visited); }

    // THREAD-BOUNDARY clone: like deep_clone BUT including STRING (append_in_place writes in place).
    // Called ONLY at thread-crossing sites (parallel/timer/ws/sse/channel/cache).
    Value clone_for_thread() const {
        std::unordered_set<const void*> visited;
        return clone_for_thread_impl(visited);
    }
    Value clone_for_thread_tracked(std::unordered_set<const void*>& visited) const {
        return clone_for_thread_impl(visited);
    }

private:
    Value deep_clone_impl(std::unordered_set<const void*>& visited) const {
        if (type_ == ARRAY && ptr_val) {
            NestingGuard g;
            auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
            if (visited.count(arr.get())) return Value(); // break the cycle — return null
            visited.insert(arr.get());
            auto v = look_new_array();
            v->reserve(arr->size());
            for (const auto& e : *arr) v->push_back(e.deep_clone_impl(visited));
            visited.erase(arr.get());
            return Value(v);
        }
        if (type_ == STRUCT && ptr_val) {
            // Field values are cloned; [0] name and [1] definition do not change → shared.
            NestingGuard g;
            auto sv = std::static_pointer_cast<std::vector<Value>>(ptr_val);
            if (visited.count(sv.get())) return Value();
            visited.insert(sv.get());
            auto v = look_new_array();
            v->reserve(sv->size());
            for (size_t i = 0; i < sv->size(); ++i)
                v->push_back(i < 2 ? (*sv)[i] : (*sv)[i].deep_clone_impl(visited));
            visited.erase(sv.get());
            return make_struct(std::move(v));
        }
        if (type_ == BYTECODE_FN && ptr_val) {
            // Closure → clone transitively (if a cloner is registered). Otherwise shallow (old behavior).
            if (auto h = bc_fn_cloner()) return h(*this, visited);
        }
        return *this; // scalar, channel, ws, sse — a shallow copy is enough (sharing is intentional)
    }

    // Thread-boundary clone (only from clone_for_thread). A copy of deep_clone_impl + the STRING
    // branch. Kept separate because deep_clone is on the HOT PATH.
    Value clone_for_thread_impl(std::unordered_set<const void*>& visited) const {
        switch (type_) {
            case INT: case FLOAT: case BOOL: case NONE:
            case CHANNEL: case WEBSOCKET: case SSE_CONN:
                return *this;   // value types + handles whose sharing is INTENTIONAL
            case STRING:
                // append_in_place mutates IN PLACE → a race on a shared capture; isolate it.
                // The map marker is recognized by address: if it were copied, the map would turn into a list (it does not change either).
                if (is_assoc_marker()) return *this;
                return ptr_val ? Value(*static_cast<const std::string*>(ptr_val.get())) : *this;
            case ARRAY: {
                if (!ptr_val) return *this;
                NestingGuard g;
                auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
                if (visited.count(arr.get())) return Value();
                visited.insert(arr.get());
                auto v = look_new_array();
                v->reserve(arr->size());
                for (const auto& e : *arr) v->push_back(e.clone_for_thread_impl(visited));
                visited.erase(arr.get());
                return Value(v);
            }
            case STRUCT: {
                if (!ptr_val) return *this;
                NestingGuard g;
                auto sv = std::static_pointer_cast<std::vector<Value>>(ptr_val);
                if (visited.count(sv.get())) return Value();
                visited.insert(sv.get());
                auto v = look_new_array();
                v->reserve(sv->size());
                for (size_t i = 0; i < sv->size(); ++i)
                    v->push_back(i < 2 ? (*sv)[i] : (*sv)[i].clone_for_thread_impl(visited));
                visited.erase(sv.get());
                return make_struct(std::move(v));
            }
            case BYTECODE_FN:
                if (ptr_val) { if (auto h = bc_fn_cloner()) return h(*this, visited); }
                return *this;
        }
        return *this; // unreachable — all Types are covered (NO default: a new type -> -Wswitch)
    }
public:

    Value operator+(const Value& o) const;
    Value operator-(const Value& o) const;
    Value operator*(const Value& o) const;
    Value operator/(const Value& o) const;
    Value operator%(const Value& o) const;
    Value pow(const Value& o)       const;
    Value concat(const Value& o)    const;
    // B7: in-place append to the accumulator string (amortized O(1)). Reduces the O(n²) behavior of copying
    // the whole string on every step of a `$s .= x` / `$s=$s.x`
    // loop to O(n). Mutation is safe because STRING is stored inline and registers are independent.
    void append_in_place(const Value& o);

    bool operator==(const Value& o) const;
    bool operator<(const Value& o)  const;
    bool operator<=(const Value& o) const;
    bool operator>(const Value& o)  const;
    bool operator>=(const Value& o) const;
    int  spaceship(const Value& o)  const;

    Value bitwise_and(const Value& o) const;
    Value bitwise_or(const Value& o)  const;
    Value bitwise_xor(const Value& o) const;
    Value bitwise_not()               const;
    Value shift_left(const Value& o)  const;
    Value shift_right(const Value& o) const;

    // ── Map (assoc) marker ────────────────────────────────────────────────────
    // Map = ["<marker>", k0, v0, k1, v1, …]. The marker is recognized by ADDRESS, not by CONTENT: a counter-less
    // (empty control block) STRING pointing at a single constant text. OLD: the marker was the TEXT "__assoc__" —
    // every list whose first element was that text counted as a map; an attacker's JSON array
    // ["__assoc__","role","admin"] turned into a role=>admin map in the app (measured). JSON/form/
    // query/LOOK code creates every text as a new object → the marker cannot be forged. Its copy is free
    // (no atomic counter); append_in_place does NOT modify a counter-less text in place (use_count()==0 → copy).
    static const std::string& assoc_marker_text() { static const std::string s("__assoc__"); return s; }
    static Value assoc_marker() {
        Value v; v.type_ = STRING;
        v.ptr_val = std::shared_ptr<void>(std::shared_ptr<void>(), const_cast<std::string*>(&assoc_marker_text()));
        return v;
    }
    bool is_assoc_marker() const { return type_ == STRING && ptr_val.get() == &assoc_marker_text(); }

    // Copy-free access to STRING content (only type_==STRING; empty for the others).
    // Internal methods (to_string/operators) use this instead of str_val.
    const std::string& str_ref() const {
        static const std::string kEmpty;
        return (type_ == STRING && ptr_val)
            ? *static_cast<const std::string*>(ptr_val.get()) : kEmpty;
    }

private:
    // B5: Value storage — scalars (int/float/bool) share a single 8-byte payload in a union;
    // STRING and all reference types sit behind ptr_val. sizeof(Value) 80→32:
    //   type_(4)+pad(4) + union(8) + shared_ptr(16). Scalar copy is trivially cheap
    //   (no string ctor/dtor, copying a null shared_ptr is non-atomic).
    Type type_ = NONE;
    union {
        int64_t int_val;
        double  float_val;
        bool    bool_val;
    };
    // STRING → shared_ptr<std::string>; ARRAY/CHANNEL/WS/SSE/BYTECODE_FN → the respective type
    std::shared_ptr<void> ptr_val;
};

// ── ValueArray — array/struct storage: non-recursive release ──────────────────
// DERIVES from std::vector<Value> (as_array/vec_ptr do not change); its only difference is the destructor. Like CPython's
// "trashcan", release depth is tracked; the elements of storage past the threshold are not destroyed but
// moved to a list and destroyed in a loop at the outermost release → stack use is independent of depth.
// OLD: when a nested array/struct dropped from its last owner, the vector→Value→vector chain
// overflowed the C++ stack (measured: segfault at ~200k depth; a single request brought down the whole process).
// The hook is NOT in ~Value but in the container: in ~Value it would ride on every element destruction (measured: +4.7%
// instructions per request); here it is once per storage. Array storage is built only with this —
// tests/value_release_test.sh rejects a source line that builds a plain std::vector<Value>.
void look_release_values(std::vector<Value>& v) noexcept;   // value.cpp

// Key index of a large map (look/assoc_index.h). Storage is still a plain list; the index is only
// a lookup accelerator and is not copied (a copy builds its own index on first lookup).
struct AssocIndex;
void look_assoc_index_free(AssocIndex* ix) noexcept;   // value.cpp

struct ValueArray : std::vector<Value> {
    using std::vector<Value>::vector;
    using std::vector<Value>::operator=;
    ValueArray() = default;
    ValueArray(const ValueArray& o) : std::vector<Value>(o) {}
    ValueArray(ValueArray&& o) noexcept : std::vector<Value>(std::move(o)) {}
    ValueArray& operator=(const ValueArray& o) { drop_index(); std::vector<Value>::operator=(o); return *this; }
    ValueArray& operator=(ValueArray&& o) noexcept { drop_index(); std::vector<Value>::operator=(std::move(o)); return *this; }
    ValueArray& operator=(const std::vector<Value>& o) { drop_index(); std::vector<Value>::operator=(o); return *this; }
    ValueArray& operator=(std::vector<Value>&& o) noexcept { drop_index(); std::vector<Value>::operator=(std::move(o)); return *this; }
    ValueArray(const std::vector<Value>& o) : std::vector<Value>(o) {}
    ValueArray(std::vector<Value>&& o) noexcept : std::vector<Value>(std::move(o)) {}
    ~ValueArray() { drop_index(); if (!empty()) look_release_values(*this); }

    // The read path builds the index lazily (only empty → built, CAS); several threads can read the same
    // shared map. The write path that CHANGES the index keeps the storage single-owner (COW).
    std::atomic<AssocIndex*> assoc_idx{nullptr};
    void drop_index() noexcept {
        if (AssocIndex* ix = assoc_idx.exchange(nullptr, std::memory_order_acq_rel)) look_assoc_index_free(ix);
    }
};

inline std::shared_ptr<std::vector<Value>> look_new_array() { return std::make_shared<ValueArray>(); }
inline std::shared_ptr<std::vector<Value>> look_new_array(const std::vector<Value>& copy) {
    return std::make_shared<ValueArray>(copy);
}

// ── LookChannel — Go-style channel (defined after Value — queue<Value> needs complete type)
struct LookChannel : std::enable_shared_from_this<LookChannel> {
    std::queue<Value>       queue;
    std::mutex              mtx;
    std::condition_variable not_empty;  // receivers wait
    std::condition_variable not_full;   // senders wait when full
    size_t                  capacity;
    bool                    closed = false;
    bool                    unbuffered = false;  // channel(0): Go rendezvous
    uint64_t                recv_gen = 0;         // increments on every recv (rendezvous)

    // Waiters inside a fiber (web request, fiber dispatch). They used to hold the THREAD with cv.wait too:
    // a request waiting on a channel stopped the other requests on the same thread
    // (keep-alive connections, suspended fibers) until the wait ended (measured: during a 400 ms
    // receive, /ok on the same worker took 350–400 ms). Now the fiber registers and suspends; the notifying
    // side (send/recv/close, the deadlock decision) wakes it on its own scheduler — the same pattern as the DB pool
    // waiter (web_stdlib.cpp ConnPool). Thread waiters (parallel() task, the
    // command line) stay on the cv. All under mtx.
    struct FiberWaiter {
        std::shared_ptr<Fiber> fiber;
        FiberScheduler*        sched;
        bool*                  queued;   // whoever removes the entry sets it false (under mtx); lives on the waiter's stack
    };
    std::vector<FiberWaiter> fiber_recv;   // not_empty waiters
    std::vector<FiberWaiter> fiber_send;   // not_full waiters (full buffer, unbuffered handoff)
    // Wake everyone in the list (called while holding mtx). Those woken retry the predicate —
    // like cv.notify_all; whoever does not win registers again.
    static void wake_fibers(std::vector<FiberWaiter>& list);

    // channel(0) = unbuffered/synchronous (Go semantics): the sender blocks until a receiver
    // takes the item. It used to be 0 = UNBOUNDED buffer → no backpressure,
    // fast producer + slow consumer = OOM. Now capacity 1 + rendezvous.
    explicit LookChannel(size_t cap = 128)
        : capacity(cap == 0 ? 1 : cap), unbuffered(cap == 0) {}

    // phase: script-end state of a parallel() task (parallel_runtime.h TaskExit::phase). While the task
    // waits it is IN_EFFECT; just before the value is DELIVERED it is moved to RUNNING (for unbuffered,
    // the moment the receiver takes it) — so a task with only its return left after delivery can count as "finished".
    void  send_val(Value val, std::atomic<int>* phase = nullptr);
    std::atomic<int>*       rv_phase = nullptr;   // unbuffered: state of the waiting sender
    Value recv_val();
    void  close_chan();
    bool  is_closed() { std::unique_lock<std::mutex> lk(mtx); return closed; }
    int   sz()        { std::unique_lock<std::mutex> lk(mtx); return (int)queue.size(); }
};

// channel([size]) — a SINGLE definition (command line + web): no size = 128, 0 = unbuffered (Go), N > 0 = N slots,
// negative or non-integer = error. It used to be 0 = UNBOUNDED on the command line and 1,048,576 slots on the web
// (the documented "unbuffered" could not be created by any path; with a slow receiver memory grew without bound).
std::shared_ptr<LookChannel> make_channel(const std::vector<Value>& args);

// ── VM callback bridge ────────────────────────────────────────────────────────
// Higher-order builtins (array::map/filter/reduce…) call the callback through the host's invoke;
// the callback is a BYTECODE_FN (VM closure). The active VM registers itself thread-locally;
// invoke delegates here. If no VM is registered, available()==false.
bool  vm_bridge_available();
Value vm_bridge_invoke(const Value& fn, std::vector<Value>& args);
// While running, the VM (vm.cpp) registers its own closure-invoker hook.
void  register_vm_bridge(Value (*hook)(const Value& fn, std::vector<Value>& args));

// ── Module ────────────────────────────────────────────────────────────────────

using NativeFn = std::function<Value(std::vector<Value>)>;

struct Module {
    std::string name;
    std::map<std::string, NativeFn> functions;
};

// ── Service registry (app::set / app::get) ────────────────────────────────────
// Holds the shared services written during setup (db, cache, config…). All
// dispatch copies share the SAME registry via shared_ptr (make_dispatch_copy).
// Mutex: setup writes from a single thread, dispatch reads from many threads; the rare in-request
// write is safe too. The lock cost is once per route, negligible next to DB I/O.
struct ServiceRegistry {
    std::mutex                   mtx;
    std::map<std::string, Value> items;
};

// ── Error system ──────────────────────────────────────────────────────────────

struct StackFrame {
    std::string function;  // function/closure name, empty = top-level
    int         line = 0;  // line the call was made at; -1 = the "… N more calls" filler row (function text)
    std::string file;      // file of the call site, "" = main script (the name is written for an included file)
};

class LookRuntimeError : public std::exception {
public:
    std::string              message;
    SourceLocation           location;
    std::vector<StackFrame>  stack;
    Value                    value;   // set by error::new() — catch e gets this Value
    bool                     has_value = false;

    LookRuntimeError(std::string msg,
                     SourceLocation loc = {},
                     std::vector<StackFrame> stk = {})
        : message(std::move(msg)), location(std::move(loc)), stack(std::move(stk)) {}

    LookRuntimeError(Value v, SourceLocation loc = {}, std::vector<StackFrame> stk = {})
        : message(v.to_string()), location(std::move(loc)), stack(std::move(stk))
        , value(std::move(v)), has_value(true) {}

    const char* what() const noexcept override { return message.c_str(); }

    std::string format() const {
        std::string out = "\nRuntime Error: " + message + "\n";
        if (location.line > 0) {
            if (!location.file.empty())
                out += "  File: " + location.file + "\n";
            out += "  Line: " + std::to_string(location.line);
            if (location.column > 0)
                out += ", Column: " + std::to_string(location.column);
            out += "\n";
        }
        if (!stack.empty()) {
            out += "Stack trace:\n";
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (it->line < 0) { out += "  " + it->function + "\n"; continue; }
                out += "  at " + (it->function.empty() ? "<main>" : it->function);
                if (it->line > 0) {
                    out += " (line " + std::to_string(it->line);
                    if (!it->file.empty()) out += " in " + it->file;
                    out += ")";
                }
                out += "\n";
            }
        }
        return out;
    }
};

class LookParseError : public std::exception {
public:
    std::string  message;
    int          line   = 0;
    int          column = 0;
    std::string  file;

    LookParseError(std::string msg, int ln = 0, int col = 0, std::string f = "")
        : message(std::move(msg)), line(ln), column(col), file(std::move(f)) {}

    const char* what() const noexcept override { return message.c_str(); }

    std::string format() const {
        std::string out = "\nParse Error: " + message + "\n";
        if (!file.empty()) out += "  File: " + file + "\n";
        if (line > 0) {
            out += "  Line: " + std::to_string(line);
            if (column > 0) out += ", Column: " + std::to_string(column);
            out += "\n";
        }
        return out;
    }
};

// exit(code) — stops the script completely, without breaking the FastCGI loop
class ExitException : public std::exception {
public:
    explicit ExitException(int code = 0) : code_(code) {}
    const char* what() const noexcept override { return "exit"; }
    int code() const { return code_; }
private:
    int code_;
};

// ── StructFieldDef ────────────────────────────────────────────────────────────

struct StructFieldDef {
    std::string name;
    std::string type;                // "" = untyped
    bool        has_default = false;
    Value       default_val;
};

} // namespace look

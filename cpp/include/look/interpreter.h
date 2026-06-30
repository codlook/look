#pragma once

#include "look/ast.h"
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace look { struct WebContext; }

namespace look {

struct Expression;
struct Statement;
struct Program;
struct BlockStatement;
struct LookFunction;

// ── Forward declarations for Value members ────────────────────────────────────
struct LookChannel;    // defined after Value (queue<Value> needs complete type)
struct WsConnection;   // defined in websocket.h
struct SseConnection;  // defined in sse.h
struct Closure;        // defined in bytecode.h — BYTECODE_FN type

// ── Value ─────────────────────────────────────────────────────────────────────

class Value {
public:
    enum Type { INT, FLOAT, STRING, BOOL, FUNCTION, ARRAY, CHANNEL, WEBSOCKET, SSE_CONN, BYTECODE_FN, NONE };

    Value()                                         : type_(NONE)     {}
    explicit Value(int i)                           : type_(INT),      int_val(i)   {}
    explicit Value(double d)                        : type_(FLOAT),    float_val(d) {}
    explicit Value(const std::string& s)            : type_(STRING),   str_val(s)   {}
    explicit Value(bool b)                          : type_(BOOL),     bool_val(b)  {}
    explicit Value(std::shared_ptr<LookFunction> f) : type_(FUNCTION), fn_val(f)   {}
    explicit Value(std::shared_ptr<std::vector<Value>> a) : type_(ARRAY),    arr_val(a)  {}
    explicit Value(std::shared_ptr<LookChannel>        c) : type_(CHANNEL),   chan_val(c) {}
    explicit Value(std::shared_ptr<WsConnection>       w) : type_(WEBSOCKET), ws_val(w)   {}
    explicit Value(std::shared_ptr<SseConnection>      s) : type_(SSE_CONN),  sse_val(s)  {}
    explicit Value(std::shared_ptr<Closure>            c) : type_(BYTECODE_FN), bc_val(c)  {}

    Type        type()      const { return type_; }
    int         as_int()    const { return int_val; }
    double      as_float()  const { return float_val; }
    std::string as_string() const { return str_val; }
    bool        as_bool()   const { return bool_val; }
    std::shared_ptr<LookFunction>        as_function() const { return fn_val;  }
    std::shared_ptr<std::vector<Value>>  as_array()    const { return arr_val; }
    std::shared_ptr<LookChannel>         as_channel()    const { return chan_val; }
    std::shared_ptr<WsConnection>        as_websocket()  const { return ws_val;  }
    std::shared_ptr<SseConnection>       as_sse()        const { return sse_val; }
    std::shared_ptr<Closure>             as_bytecode_fn()const { return bc_val;  }

    std::string to_string() const;
    double      to_float()  const;
    int         to_int()    const;
    bool        is_truthy() const;

    // ARRAY için recursive deep copy; döngüsel referans güvenli (visited set ile kırılır)
    Value deep_clone() const {
        std::unordered_set<const void*> visited;
        return deep_clone_impl(visited);
    }

private:
    Value deep_clone_impl(std::unordered_set<const void*>& visited) const {
        if (type_ == ARRAY && arr_val) {
            if (visited.count(arr_val.get())) return Value(); // döngü kır — null döndür
            visited.insert(arr_val.get());
            auto v = std::make_shared<std::vector<Value>>();
            v->reserve(arr_val->size());
            for (const auto& e : *arr_val) v->push_back(e.deep_clone_impl(visited));
            visited.erase(arr_val.get());
            return Value(v);
        }
        return *this; // scalar, fn, ws, sse — shallow copy yeterli
    }
public:

    Value operator+(const Value& o) const;
    Value operator-(const Value& o) const;
    Value operator*(const Value& o) const;
    Value operator/(const Value& o) const;
    Value operator%(const Value& o) const;
    Value pow(const Value& o)       const;
    Value concat(const Value& o)    const;

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

private:
    Type        type_;
    int         int_val   = 0;
    double      float_val = 0.0;
    std::string str_val;
    bool        bool_val  = false;
    std::shared_ptr<LookFunction>        fn_val;
    std::shared_ptr<std::vector<Value>>  arr_val;
    std::shared_ptr<LookChannel>         chan_val;
    std::shared_ptr<WsConnection>        ws_val;
    std::shared_ptr<SseConnection>       sse_val;
    std::shared_ptr<Closure>             bc_val;
};

// ── LookChannel — Go-style channel (defined after Value — queue<Value> needs complete type)
struct LookChannel {
    std::queue<Value>       queue;
    std::mutex              mtx;
    std::condition_variable not_empty;  // receivers wait
    std::condition_variable not_full;   // senders wait when full
    size_t                  capacity;
    bool                    closed = false;

    explicit LookChannel(size_t cap = 128)
        : capacity(cap == 0 ? (size_t)-1 : cap) {}

    void  send_val(Value val);
    Value recv_val();
    void  close_chan();
    bool  is_closed() { std::unique_lock<std::mutex> lk(mtx); return closed; }
    int   sz()        { std::unique_lock<std::mutex> lk(mtx); return (int)queue.size(); }
};

// ── Environment ───────────────────────────────────────────────────────────────

class Environment {
public:
    explicit Environment(std::shared_ptr<Environment> parent = nullptr)
        : parent_(parent) {}

    void define(const std::string& name, const Value& val) { values_[name] = val; }

    Value get(const std::string& name) const {
        auto it = values_.find(name);
        if (it != values_.end()) return it->second;
        if (parent_)             return parent_->get(name);
        throw std::runtime_error("Undefined variable: " + name);
    }

    void set(const std::string& name, const Value& val) {
        auto it = values_.find(name);
        if (it != values_.end()) { it->second = val; return; }
        if (parent_)             { parent_->set(name, val); return; }
        throw std::runtime_error("Undefined variable: " + name);
    }

    // Included file'ların function/const tanımlarını caller scope'a aktarmak için
    const std::map<std::string, Value>& entries() const { return values_; }

    // Dispatch kopyası için derin kopya — shared mutable state race kondisyonunu engeller
    std::shared_ptr<Environment> clone() const {
        auto e = std::make_shared<Environment>(parent_);
        for (const auto& [k, v] : values_)
            e->values_[k] = v.deep_clone();
        return e;
    }

private:
    std::map<std::string, Value> values_;
    std::shared_ptr<Environment> parent_;
};

// ── LookFunction ──────────────────────────────────────────────────────────────

struct LookFunction {
    std::string name;
    std::vector<std::string> parameters;
    bool is_variadic = false;
    const BlockStatement* body;
    std::shared_ptr<Environment> closure;

    LookFunction(std::string n, std::vector<std::string> p, bool variadic,
                 const BlockStatement* b, std::shared_ptr<Environment> c)
        : name(std::move(n)), parameters(std::move(p)), is_variadic(variadic),
          body(b), closure(std::move(c)) {}
};

// ── Module ────────────────────────────────────────────────────────────────────

using NativeFn = std::function<Value(std::vector<Value>)>;

struct Module {
    std::string name;
    std::map<std::string, NativeFn> functions;
};

// ── Error system ──────────────────────────────────────────────────────────────

struct StackFrame {
    std::string function;  // function/closure name, empty = top-level
    int         line = 0;
};

class LookRuntimeError : public std::exception {
public:
    std::string              message;
    SourceLocation           location;
    std::vector<StackFrame>  stack;

    LookRuntimeError(std::string msg,
                     SourceLocation loc = {},
                     std::vector<StackFrame> stk = {})
        : message(std::move(msg)), location(std::move(loc)), stack(std::move(stk)) {}

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
                out += "  at " + (it->function.empty() ? "<main>" : it->function);
                if (it->line > 0)
                    out += " (line " + std::to_string(it->line) + ")";
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

// ── Exceptions ────────────────────────────────────────────────────────────────

class ReturnException : public std::exception {
public:
    explicit ReturnException(const Value& v) : value_(v) {}
    const char* what() const noexcept override { return "return"; }
    Value value() const { return value_; }
private:
    Value value_;
};

class BreakException : public std::exception {
    const char* what() const noexcept override { return "break"; }
};
class ContinueException : public std::exception {
    const char* what() const noexcept override { return "continue"; }
};

// exit(code) — script'i tamamen durdurur, FastCGI döngüsünü bozmaz
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
    bool        has_default = false;
    Value       default_val;
};

// ── RouteEntry — warm start icin kayitli route ────────────────────────────────

struct RouteEntry {
    std::string              method;       // "GET", "POST", "404", "*"
    std::string              pattern;      // "/menu/{slug}"
    std::vector<std::string> param_names;  // ["slug"]
    std::regex               pattern_re;   // compiled regex
    Value                    callback;     // captured closure
    bool                     is_404 = false;
};

// ── Interpreter ───────────────────────────────────────────────────────────────

class Interpreter {
public:
    Interpreter();
    explicit Interpreter(std::ostream& out);
    void  interpret(const Program& program);
    void  set_web_context(WebContext* ctx);
    void  set_output(std::ostream& out) { output_stream_ = &out; }
    void  set_output(std::ostream* out) { output_stream_ = out; }

    // REPL support
    using ReplValueCallback = std::function<void(const Value&)>;
    void set_repl_value_callback(ReplValueCallback cb) { repl_value_cb_ = std::move(cb); }
    void clear_repl_value_callback() { repl_value_cb_ = nullptr; }

    // Returns names of all user-defined globals (excludes internal names starting with __)
    std::vector<std::string> get_global_names() const;

    // Test runner helpers — register a module for `use X;` and a top-level built-in
    void register_use_module(const std::string& name, Module mod) {
        stdlib_[name] = std::move(mod);
    }
    void register_builtin(const std::string& name, std::function<Value(std::vector<Value>)> fn);
    std::ostream& output() const { return *output_stream_; }

    // Warm start: setup_mode=true → route() sadece kaydeder, dispatch yapmaz
    void set_setup_mode(bool m) { setup_mode_ = m; }
    bool is_setup_mode()  const { return setup_mode_; }

    // Warm start dispatch: kayitli route'lardan eslesen handler'i cagir
    // web_ctx_ onceden set_web_context() ile atanmis olmali.
    void dispatch_routes();

    Value invoke(const Value& fn, std::vector<Value> args);
    Value get_global(const std::string& name) {
        try { return globals_->get(name); } catch (...) { return Value(); }
    }

    // runtime:: stats
    int  get_route_count()   const { return (int)route_registry_.size(); }
    int  get_request_count() const { return request_count_.load(); }
    long get_uptime_sec()    const {
        auto now = std::chrono::steady_clock::now();
        return (long)std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    }

    static constexpr int MAX_CALL_DEPTH = 500;

    void set_file(const std::string& file) { current_file_ = file; if (main_script_.empty()) main_script_ = file; }

    // Phase 15: WebSocket — set active WS connection for dispatch_routes() "WS" matching
    void set_ws_connection(std::shared_ptr<WsConnection> ws) { ws_conn_ = std::move(ws); }

    // Phase 16: SSE — set active SSE connection for dispatch_routes() "SSE" matching
    void set_sse_connection(std::shared_ptr<SseConnection> sse) { sse_conn_ = std::move(sse); }

    // Thread-safe dispatch copy — shares read-only setup state, has fresh per-request state.
    // Call set_output() and set_web_context() on the copy before dispatch_routes().
    std::unique_ptr<Interpreter> make_dispatch_copy() const;

    // VM builtin wiring: set_web_context() sonrası modules_'dan fonksiyon al.
    // module = "response", fn = "header" → modules_["response"].functions["header"]
    // Bulunamazsa null std::function döner.
    NativeFn get_module_fn(const std::string& module_name, const std::string& fn_name) const;

    // VM builtin wiring: route_registry_ erişimi — VM dispatch için closure ptr'ları al.
    const std::vector<RouteEntry>& get_route_registry() const { return route_registry_; }

    // Test runner support
    struct TestCase {
        std::string name;
        Value       fn;
    };
    void register_test_case(const std::string& name, const Value& fn) {
        test_cases_.push_back({name, fn});
    }
    const std::vector<TestCase>& test_cases() const { return test_cases_; }
    void set_before_each(const Value& fn) { before_each_ = fn; }
    void set_after_each(const Value& fn)  { after_each_  = fn; }
    const Value& before_each() const { return before_each_; }
    const Value& after_each()  const { return after_each_; }

private:
    std::shared_ptr<Environment> globals_;
    std::shared_ptr<Environment> current_;
    std::ostream* output_stream_;
    std::map<std::string, Module> modules_;
    std::map<std::string, Module> stdlib_;
    int call_depth_ = 0;
    WebContext* web_ctx_ = nullptr;

    // Error tracking
    std::string              main_script_;    // proje kök scripti — değişmez
    std::string              current_file_;
    SourceLocation           current_loc_;
    std::vector<StackFrame>  call_stack_;

    // Warm start state
    bool                    setup_mode_ = false;
    std::vector<RouteEntry> route_registry_;

    // Phase 11: struct definitions + iota counter
    std::map<std::string, std::vector<StructFieldDef>> struct_defs_;

    // Phase 18.5: dosya modül sistemi
    // included_files_: döngüsel include koruması (abs path set)
    // owned_programs_: include edilen dosyaların AST'leri — fonksiyon body pointer'ları bu AST'lere bakıyor
    std::unordered_set<std::string>       included_files_;
    std::vector<std::unique_ptr<Program>> owned_programs_;

    // REPL value callback — fires when an expression statement produces a value
    ReplValueCallback repl_value_cb_;

    // Test runner — registered via test() built-in
    std::vector<TestCase> test_cases_;
    Value before_each_;
    Value after_each_;
    std::map<std::string, std::function<Value(std::vector<Value>)>> builtins_;

    // Phase 15: active WebSocket connection (set before dispatch_routes() for WS routes)
    std::shared_ptr<WsConnection>  ws_conn_;
    // Phase 16: active SSE connection (set before dispatch_routes() for SSE routes)
    std::shared_ptr<SseConnection> sse_conn_;
    int current_iota_ = 0;

    // runtime:: stats
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    std::atomic<int>                       request_count_{0};

    void  execute_statement(const Statement& stmt);
    void  execute_block(const BlockStatement& block, std::shared_ptr<Environment> enclosing);
    Value evaluate_expression(const Expression& expr);
    Value call_function(std::shared_ptr<LookFunction> fn, std::vector<Value> args);
    std::string interpolate_string(const std::string& raw);
    std::string build_output(const std::vector<std::unique_ptr<Expression>>& exprs);
};

// .env-aware env() — VM setup builtins için (g_env_vars + system env)
std::string look_get_env(const std::string& key, const std::string& default_val = "");

} // namespace look

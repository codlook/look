#pragma once

#include "look/bytecode.h"
#include "look/interpreter.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <sstream>

namespace look {

// ── VM runtime hata ───────────────────────────────────────────────────────────

class LookVmError : public std::runtime_error {
public:
    int line;
    explicit LookVmError(const std::string& msg, int line = 0)
        : std::runtime_error(msg), line(line) {}
};

// ── BuiltinFn ─────────────────────────────────────────────────────────────────

using BuiltinFn = std::function<Value(std::vector<Value>&)>;

// ── VM ────────────────────────────────────────────────────────────────────────

class VM {
public:
    struct SharedState {
        const std::unordered_map<std::string, Value>*            globals     = nullptr;
        const std::vector<std::pair<std::string, Closure*>>*     routes      = nullptr;
        const std::unordered_map<std::string,
              std::vector<StructFieldDef>>*                       struct_defs = nullptr;
        // builtins: per-request olarak set edilir (set_builtins ile)
        const std::vector<BuiltinFn>*                            builtins    = nullptr;
    };

    explicit VM(SharedState shared, std::ostream& output);

    void set_globals(std::unordered_map<std::string, Value> g);
    void set_web_context(WebContext* ctx);
    void set_ws_connection(std::shared_ptr<WsConnection> ws);
    void set_sse_connection(std::shared_ptr<SseConnection> sse);
    // Per-request builtins — her request'te çağrılmalı (module fn'ler ctx'e bağlıdır)
    void set_builtins(const std::vector<BuiltinFn>* b) { shared_.builtins = b; }

    void  execute(const CompiledProgram& prog);
    void  dispatch_routes(const std::string& method, const std::string& path);
    Value call_closure(const Closure& closure, std::vector<Value> args);
    const std::unordered_map<std::string, Value>& get_globals() const { return globals_; }

private:
    static constexpr int MAX_CALL_DEPTH = 500;

    std::vector<Value> regs_;

    struct Frame {
        const FunctionProto* proto;
        const Closure*       closure;
        int                  ip;
        int                  base;
        int                  ret_reg;
    };
    std::vector<Frame> call_stack_;

    struct TryCatchEntry { int catch_ip; int frame_depth; int reg_base; };
    std::vector<TryCatchEntry> try_stack_;
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
    Value       get_field(const Value& obj, const std::string& field);
    void        set_field(Value& obj, const std::string& field, const Value& val);

    // Shared read-only state
    SharedState                              shared_;
    std::unordered_map<std::string, Value>   globals_;
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

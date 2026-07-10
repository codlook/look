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

// Route-level middleware destekli VM route girişi
struct VmRoute {
    std::string          pattern;     // "GET:/path/{id}"
    Closure*             fn;          // route handler closure
    std::vector<Closure*> middlewares; // route-level middleware listesi (setup'ta doldurulur)
    int                  app_index = -1; // g_http_app.vm_routes'taki kalıcı indeks —
                                         // VM hatasında route'u kalıcı interpreter'a
                                         // sabitlemek için kullanılır
};

// VM'den çıkarılmış route'a istek geldi — sessiz interpreter fallback sinyali.
// (Hata DEĞİL: route daha önce VM'de başarısız oldu ve kalıcı sabitlendi.)
class VmRouteDisabled : public std::exception {
public:
    const char* what() const noexcept override { return "route interpreter'a sabitli"; }
};

// ── VM ────────────────────────────────────────────────────────────────────────

class VM {
public:
    struct SharedState {
        const std::unordered_map<std::string, Value>*            globals     = nullptr;
        const std::vector<VmRoute>*                              routes      = nullptr;
        const std::unordered_map<std::string,
              std::vector<StructFieldDef>>*                       struct_defs = nullptr;
        // builtins: per-request olarak set edilir (set_builtins ile)
        const std::vector<BuiltinFn>*                            builtins    = nullptr;
        // Route bazında kalıcı VM-devre-dışı bayrakları (app_index ile indekslenir).
        // Eşleşen route'un bayrağı setli ise dispatch VmRouteDisabled fırlatır →
        // caller sessizce interpreter'a düşer.
        const std::vector<uint8_t>*                              route_disabled = nullptr;
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
    // Son dispatch'te eşleşen route'un app_index'i (-1 = eşleşme yok/404).
    // VM hatasında hangi route'un interpreter'a sabitleneceğini söyler.
    int   last_matched_route() const { return last_matched_route_; }
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

    int last_matched_route_ = -1;

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

#pragma once

#include "look/ast.h"
#include <cstdint>
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

namespace look { struct WebContext;

// Double → kısa round-trip string (std::to_chars shortest). to_string() ve
// JSON serileştirme ortak kullanır — bilimsel gösterim/veri kaybını önler.
std::string look_format_double(double d); }

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

    // B5: skalerler union'da (8 byte), STRING pointer arkasında → sizeof 80→32,
    // skaler kopyada string ctor/dtor yok. String literal'leri constant pool'dan
    // LOAD_CONST ile paylaşılır (refcount-bump, kopya yok) → RAM + hız.
    Value()                                         : type_(NONE),     int_val(0)   {}
    explicit Value(int64_t i)                       : type_(INT),      int_val(i)   {}
    explicit Value(int i)                           : type_(INT),      int_val((int64_t)i) {}
    explicit Value(double d)                        : type_(FLOAT),    float_val(d) {}
    explicit Value(bool b)                          : type_(BOOL),     bool_val(b)  {}
    explicit Value(const std::string& s)            : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(s)) {}
    explicit Value(std::string&& s)                 : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(std::move(s))) {}
    explicit Value(std::shared_ptr<LookFunction> f) : type_(FUNCTION),    int_val(0), ptr_val(std::move(f)) {}
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
    // Tek shared_ptr<void>'dan tipli erişim — type_ hangi tipin aktif olduğunu garanti eder.
    std::shared_ptr<LookFunction>        as_function() const { return std::static_pointer_cast<LookFunction>(ptr_val); }
    std::shared_ptr<std::vector<Value>>  as_array()    const { return std::static_pointer_cast<std::vector<Value>>(ptr_val); }
    std::shared_ptr<LookChannel>         as_channel()    const { return std::static_pointer_cast<LookChannel>(ptr_val); }
    std::shared_ptr<WsConnection>        as_websocket()  const { return std::static_pointer_cast<WsConnection>(ptr_val); }
    std::shared_ptr<SseConnection>       as_sse()        const { return std::static_pointer_cast<SseConnection>(ptr_val); }
    std::shared_ptr<Closure>             as_bytecode_fn()const { return std::static_pointer_cast<Closure>(ptr_val); }

    std::string to_string() const;
    double      to_float()  const;
    int64_t     to_int()    const;
    bool        is_truthy() const;

    // ARRAY için recursive deep copy; döngüsel referans güvenli (visited set ile kırılır)
    Value deep_clone() const {
        std::unordered_set<const void*> visited;
        return deep_clone_impl(visited);
    }

    // BYTECODE_FN (Closure) transitif deep-clone hook'u. interpreter.h Closure'ı GÖREMEZ
    // (bytecode.h bunu sonra include eder) → vm katmanı bir cloner kaydeder. Kayıtsızsa
    // eski davranış (shallow). 58 parallel: bir closure yakalanınca, deep_clone onu shallow
    // kopyalarsa closure-içi cell'ler parent'la PAYLAŞIMLI kalır → veri yarışı (np1). Cloner
    // closure'ı klonlayıp cell-capture'larını + iç closure'ları özyineli klonlar.
    using BcFnCloner = Value(*)(const Value&, std::unordered_set<const void*>&);
    static BcFnCloner& bc_fn_cloner() { static BcFnCloner h = nullptr; return h; }
    // Hook özyinelemesi için visited-paylaşımlı deep-clone (deep_clone_impl private).
    Value deep_clone_tracked(std::unordered_set<const void*>& visited) const { return deep_clone_impl(visited); }

    // FUNCTION (tree-walk interpreter closure = LookFunction + Environment) hook'u. interpreter.h
    // LookFunction'ı TAM göremez (aşağıda tanımlı) → interpreter.cpp bir cloner kaydeder.
    using FnCloner = Value(*)(const Value&, std::unordered_set<const void*>&);
    static FnCloner& fn_cloner() { static FnCloner h = nullptr; return h; }

    // THREAD-SINIRI klonu: deep_clone gibi AMA FUNCTION + STRING dahil. ÖNEMLİ: deep_clone()
    // SICAK YOLDA (Environment::clone → make_dispatch_copy, HER WS MESAJINDA) çağrıldığı için
    // ona FUNCTION dalı EKLENMEZ (her mesajda tüm global fn'leri derin klonlar = felaket).
    // Bu ayrı yol YALNIZ thread-crossing site'larda (parallel/timer/ws/sse/channel/cache)
    // çağrılır — deep_clone semantiği değişmez, sıcak yol maliyeti aynı kalır.
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
            auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
            if (visited.count(arr.get())) return Value(); // döngü kır — null döndür
            visited.insert(arr.get());
            auto v = std::make_shared<std::vector<Value>>();
            v->reserve(arr->size());
            for (const auto& e : *arr) v->push_back(e.deep_clone_impl(visited));
            visited.erase(arr.get());
            return Value(v);
        }
        if (type_ == BYTECODE_FN && ptr_val) {
            // Closure → transitif klonla (cloner kayıtlıysa). Yoksa shallow (eski).
            if (auto h = bc_fn_cloner()) return h(*this, visited);
        }
        return *this; // scalar, channel, ws, sse — shallow copy yeterli (paylaşım kasıtlı)
    }

    // Thread-sınırı klonu (yalnız clone_for_thread'den). deep_clone_impl'in kopyası +
    // FUNCTION (interpreter closure) + STRING dalları. deep_clone SICAK YOLDA olduğu için
    // ayrı tutuluyor; buraya eklenen dallar sıcak yolu ETKİLEMEZ.
    Value clone_for_thread_impl(std::unordered_set<const void*>& visited) const {
        switch (type_) {
            case INT: case FLOAT: case BOOL: case NONE:
            case CHANNEL: case WEBSOCKET: case SSE_CONN:
                return *this;   // değer tipleri + paylaşımı KASITLI handle'lar
            case STRING:
                // append_in_place YERİNDE mutasyon → paylaşılan capture'da yarış; izole et.
                return ptr_val ? Value(*static_cast<const std::string*>(ptr_val.get())) : *this;
            case ARRAY: {
                if (!ptr_val) return *this;
                auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
                if (visited.count(arr.get())) return Value();
                visited.insert(arr.get());
                auto v = std::make_shared<std::vector<Value>>();
                v->reserve(arr->size());
                for (const auto& e : *arr) v->push_back(e.clone_for_thread_impl(visited));
                visited.erase(arr.get());
                return Value(v);
            }
            case BYTECODE_FN:
                if (ptr_val) { if (auto h = bc_fn_cloner()) return h(*this, visited); }
                return *this;
            case FUNCTION:
                // interpreter.cpp'de kayıtlı: LookFunction + closure Environment klonu.
                if (ptr_val) { if (auto h = fn_cloner()) return h(*this, visited); }
                return *this;
        }
        return *this; // unreachable — tüm Type'lar kapsandı (default YOK: yeni tip -> -Wswitch)
    }
public:

    Value operator+(const Value& o) const;
    Value operator-(const Value& o) const;
    Value operator*(const Value& o) const;
    Value operator/(const Value& o) const;
    Value operator%(const Value& o) const;
    Value pow(const Value& o)       const;
    Value concat(const Value& o)    const;
    // B7: akümülatör string'ine yerinde ekleme (amortize O(1)). `$s .= x` / `$s=$s.x`
    // döngüsünde her adımda tüm string'i kopyalayan O(n²) davranışı O(n)'e indirir.
    // STRING inline saklandığı ve register'lar bağımsız olduğu için mutasyon güvenli.
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

    // STRING içeriğine kopyasız erişim (yalnız type_==STRING; diğerlerinde boş).
    // İç metodlar (to_string/operatörler) str_val yerine bunu kullanır.
    const std::string& str_ref() const {
        static const std::string kEmpty;
        return (type_ == STRING && ptr_val)
            ? *static_cast<const std::string*>(ptr_val.get()) : kEmpty;
    }

private:
    // B5: Value depolama — skalerler (int/float/bool) union'da tek 8-byte payload;
    // STRING ve tüm referans tipleri ptr_val arkasında. sizeof(Value) 80→32:
    //   type_(4)+pad(4) + union(8) + shared_ptr(16). Skaler kopya trivial-ucuz
    //   (string ctor/dtor yok, null shared_ptr kopyası atomiksiz).
    Type type_ = NONE;
    union {
        int64_t int_val;
        double  float_val;
        bool    bool_val;
    };
    // STRING → shared_ptr<std::string>; ARRAY/FUNCTION/CHANNEL/WS/SSE/BYTECODE_FN → ilgili tip
    std::shared_ptr<void> ptr_val;
};

// ── LookChannel — Go-style channel (defined after Value — queue<Value> needs complete type)
struct LookChannel {
    std::queue<Value>       queue;
    std::mutex              mtx;
    std::condition_variable not_empty;  // receivers wait
    std::condition_variable not_full;   // senders wait when full
    size_t                  capacity;
    bool                    closed = false;
    bool                    unbuffered = false;  // channel(0): Go rendezvous
    uint64_t                recv_gen = 0;         // her recv'de artar (rendezvous)

    // channel(0) = unbuffered/senkron (Go semantiği): gönderici alıcı öğeyi
    // alana dek bloke olur. Eskiden 0 = SINIRSIZ buffer'dı → backpressure yok,
    // hızlı üretici + yavaş tüketici = OOM. Artık kapasite 1 + rendezvous.
    explicit LookChannel(size_t cap = 128)
        : capacity(cap == 0 ? 1 : cap), unbuffered(cap == 0) {}

    void  send_val(Value val);
    Value recv_val();
    void  close_chan();
    bool  is_closed() { std::unique_lock<std::mutex> lk(mtx); return closed; }
    int   sz()        { std::unique_lock<std::mutex> lk(mtx); return (int)queue.size(); }
};

// ── VM/interpreter callback köprüsü ───────────────────────────────────────────
// Higher-order builtin'ler (array::map/filter/reduce…) callback'i interpreter'ın
// invoke'uyla çağırıyor; VM route'unda callback bir BYTECODE_FN (VM closure) olur.
// Aktif VM kendini thread-local kaydeder; interpreter BYTECODE_FN gelince buraya
// delege eder. vm.cpp tanımlar. Kayıtlı VM yoksa available()==false → fallback.
bool  vm_bridge_available();
Value vm_bridge_invoke(const Value& fn, std::vector<Value>& args);
// VM (vm.cpp) çalışırken kendi closure-invoker hook'unu kaydeder. CLI (tree-walk)
// bunu hiç çağırmaz → hook null → vm_bridge_available()==false.
void  register_vm_bridge(Value (*hook)(const Value& fn, std::vector<Value>& args));

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
        // Fonksiyon sınırını YAZMA için geçme: dıştaki (global) değişkeni implicit
        // ezmeyi engeller. Bulunamazsa çağıran define() ile local yaratır — Python
        // semantiği: okuma (get) yukarı düşer, yazma fonksiyon içinde local kalır.
        // Blok sınırları şeffaftır (fn_boundary_ yalnız fonksiyon çağrısında set).
        if (fn_boundary_ || !parent_)
            throw std::runtime_error("Undefined variable: " + name);
        parent_->set(name, val);
    }

    void mark_fn_boundary() { fn_boundary_ = true; }

    // Included file'ların function/const tanımlarını caller scope'a aktarmak için
    const std::map<std::string, Value>& entries() const { return values_; }

    // Dispatch kopyası için derin kopya — shared mutable state race kondisyonunu engeller
    std::shared_ptr<Environment> clone() const {
        auto e = std::make_shared<Environment>(parent_);
        for (const auto& [k, v] : values_)
            e->values_[k] = v.deep_clone();
        return e;
    }

    // THREAD-SINIRI klonu: clone() gibi AMA değerleri clone_for_thread ile klonlar (FUNCTION
    // dahil). clone() `deep_clone` kullanır ve o FUNCTION'ı bilmez → İÇ İÇE closure capture'ı
    // (`use ($inner)` — $inner kendisi closure) ikinci seviyede sığ kalıp $big'i paylaşıyordu
    // (t8 interp). parent_ PAYLAŞIMLI kalır (closure'ın parent'ı setup-zamanı globals_;
    // klonlarsak global okumaları bozulur). visited sınır ötesine taşınır → döngü koruması.
    std::shared_ptr<Environment> clone_for_thread(std::unordered_set<const void*>& visited) const {
        auto e = std::make_shared<Environment>(parent_);
        for (const auto& [k, v] : values_)
            e->values_[k] = v.clone_for_thread_tracked(visited);
        e->fn_boundary_ = fn_boundary_;
        return e;
    }

private:
    std::map<std::string, Value> values_;
    std::shared_ptr<Environment> parent_;
    bool fn_boundary_ = false;   // true: yazma (set) bu env'i geçip global'e ulaşamaz
};

// ── LookFunction ──────────────────────────────────────────────────────────────

struct LookFunction {
    std::string name;
    std::vector<std::string> parameters;
    bool is_variadic = false;
    const BlockStatement* body;
    std::shared_ptr<Environment> closure;
    // Varsayılan parametre ifadeleri (AST'den ödünç; paralel, nullptr = yok).
    const std::vector<std::unique_ptr<Expression>>* defaults = nullptr;

    LookFunction(std::string n, std::vector<std::string> p, bool variadic,
                 const BlockStatement* b, std::shared_ptr<Environment> c,
                 const std::vector<std::unique_ptr<Expression>>* defs = nullptr)
        : name(std::move(n)), parameters(std::move(p)), is_variadic(variadic),
          body(b), closure(std::move(c)), defaults(defs) {}
};

// ── Module ────────────────────────────────────────────────────────────────────

using NativeFn = std::function<Value(std::vector<Value>)>;

struct Module {
    std::string name;
    std::map<std::string, NativeFn> functions;
};

// ── Servis kaydı (app::set / app::get) ────────────────────────────────────────
// Setup'ta yazılan paylaşılan servisleri (db, cache, config…) tutar. Tüm
// dispatch kopyaları shared_ptr ile AYNI registry'yi paylaşır (make_dispatch_copy).
// Mutex: setup tek thread yazar, dispatch çok thread okur; nadir request-içi
// yazma da güvenli. Kilit maliyeti route başına 1 kez, DB I/O yanında ihmal.
struct ServiceRegistry {
    std::mutex                   mtx;
    std::map<std::string, Value> items;
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
    std::vector<Value>       middlewares;  // route-level middleware listesi (setup'ta doldurulur)
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

    // Tree-walk interpreter her LOOK çağrısında çok sayıda native C++ frame
    // kullanır (~16KB/çağrı). 8 MB thread stack'te native limit ~500 civarı; 500
    // guard'ı tam limitte olduğu için guard ateşlerken throw'un kendisi stack'i
    // taşırıp çökertiyordu (STATUS_STACK_OVERFLOW, Linux worker'ları dahil). 256
    // native limitin güvenli ~2x altında → guard temiz ateşler, yakalanabilir hata.
    // (VM heap-based call_stack kullandığı için kendi 500 limiti güvenli kalır.)
    static constexpr int MAX_CALL_DEPTH = 256;

    void set_file(const std::string& file) { current_file_ = file; if (main_script_.empty()) main_script_ = file; }

    // Phase 15: WebSocket — set active WS connection for dispatch_routes() "WS" matching
    void set_ws_connection(std::shared_ptr<WsConnection> ws) { ws_conn_ = std::move(ws); }

    // Phase 16: SSE — set active SSE connection for dispatch_routes() "SSE" matching
    void set_sse_connection(std::shared_ptr<SseConnection> sse) { sse_conn_ = std::move(sse); }

    // Thread-safe dispatch copy — shares read-only setup state, has fresh per-request state.
    // Call set_output() and set_web_context() on the copy before dispatch_routes().
    std::unique_ptr<Interpreter> make_dispatch_copy() const;

    // Dispatch kopyası worker thread'inde YENİDEN KULLANILIYOR (per-request kurulum
    // 30-63µs'ydi — route'u çalıştırmaktan pahalı). Kopya paylaşıldığı için, kullanıcı
    // kodu interpreter'da çalışacaksa globals ÖNCE taze snapshot'a dönmeli — yoksa bir
    // isteğin yazdığı global sonraki isteğe SIZAR. VM route'u interpreter globals'ını
    // kullanmadığından bu maliyeti ödemez (yalnız fallback/interpreter yolu çağırır).
    void reset_globals_from(const Interpreter& base) {
        globals_ = base.globals_->clone();
        current_ = std::make_shared<Environment>(globals_);
    }

    // VM builtin wiring: set_web_context() sonrası modules_'dan fonksiyon al.
    // module = "response", fn = "header" → modules_["response"].functions["header"]
    // Bulunamazsa null std::function döner.
    NativeFn get_module_fn(const std::string& module_name, const std::string& fn_name) const;

    // C9 (CLI-VM): bir stdlib modülünü modules_'a yükler (use semantiği) — VM builtin
    // wiring get_module_fn ile bunları görebilsin diye. Dış/paket modüller kapsam dışı
    // → false (caller tree-walk'a düşer). Default CLI'ı etkilemez (opt-in yol).
    bool load_stdlib_module(const std::string& name);

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

    // Servis kaydı — app::set/get (setup'ta yazılır, tüm dispatch'lerce paylaşılır)
    std::shared_ptr<ServiceRegistry> services_ = std::make_shared<ServiceRegistry>();
    void register_app_module();   // constructor'da çağrılır — app:: core modülü
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
    std::vector<Value>      before_route_registry_;  // before_route() middleware listesi

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

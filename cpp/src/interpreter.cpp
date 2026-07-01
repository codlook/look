#include "look/interpreter.h"
#include "look/stdlib.h"
#include "look/web.h"
#include "look/parallel_runtime.h"
#include "look/logger.h"
#include "look/ast.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/websocket.h"
#include "look/sse.h"
#include "look/timer.h"
#include "look/jobs_store.h"
#include <regex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace interp_fs = std::filesystem;

namespace look {

// â”€â”€ Value â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::string Value::to_string() const {
    switch (type_) {
        case INT:    return std::to_string(int_val);
        case FLOAT: {
            std::ostringstream oss;
            oss.imbue(std::locale::classic()); // locale-bağımsız: "3.5" değil "3,5"
            oss << float_val;
            return oss.str();
        }
        case STRING: return str_val;
        case BOOL:   return bool_val ? "true" : "false";
        case NONE:   return "null";
        case ARRAY: {
            std::string s = "[";
            for (size_t i = 0; i < arr_val->size(); ++i) {
                if (i) s += ", ";
                s += (*arr_val)[i].to_string();
            }
            return s + "]";
        }
        case FUNCTION:  return "<function>";
        case CHANNEL:   return "<channel>";
        case WEBSOCKET: return "<websocket>";
    }
    return "";
}

bool Value::is_truthy() const {
    switch (type_) {
        case BOOL:   return bool_val;
        case INT:    return int_val != 0;
        case FLOAT:  return float_val != 0.0;
        case STRING: return !str_val.empty() && str_val != "0";
        case NONE:   return false;
        case ARRAY:   return !arr_val->empty();
        case CHANNEL:   return chan_val != nullptr;
        case WEBSOCKET: return ws_val  != nullptr;
        default:        return true;
    }
}

double Value::to_float() const {
    switch (type_) {
        case INT:    return (double)int_val;
        case FLOAT:  return float_val;
        case STRING: try { return std::stod(str_val); } catch(...) { return 0.0; }
        case BOOL:   return bool_val ? 1.0 : 0.0;
        default:     return 0.0;
    }
}

int Value::to_int() const {
    switch (type_) {
        case INT:    return int_val;
        case FLOAT:  return (int)float_val;
        case STRING: try { return std::stoi(str_val); } catch(...) { return 0; }
        case BOOL:   return bool_val ? 1 : 0;
        default:     return 0;
    }
}

Value Value::operator+(const Value& o) const {
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() + o.to_float());
    return Value(int_val + o.to_int());
}
Value Value::operator-(const Value& o) const {
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() - o.to_float());
    return Value(int_val - o.to_int());
}
Value Value::operator*(const Value& o) const {
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() * o.to_float());
    return Value(int_val * o.to_int());
}
Value Value::operator/(const Value& o) const {
    double d = o.to_float();
    if (d == 0.0) throw std::runtime_error("Division by zero");  // caught and enriched by interpreter
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() / d);
    int i = o.to_int();
    if (i != 0 && int_val % i == 0) return Value(int_val / i);
    return Value(to_float() / d);
}
Value Value::operator%(const Value& o) const {
    int i = o.to_int();
    if (i == 0) throw std::runtime_error("Modulo by zero");
    return Value(to_int() % i);
}
Value Value::pow(const Value& o) const { return Value(std::pow(to_float(), o.to_float())); }
Value Value::concat(const Value& o)    const { return Value(to_string() + o.to_string()); }

bool Value::operator==(const Value& o) const {
    if (type_ == NONE && o.type_ == NONE) return true;
    if (type_ == NONE || o.type_ == NONE) return false; // null sadece null'a esit
    if (type_ == ARRAY || o.type_ == ARRAY) return false; // array karsilastirmasi desteklenmiyor
    if (type_ == BOOL || o.type_ == BOOL) return is_truthy() == o.is_truthy();
    if (type_ == STRING && o.type_ == STRING) return str_val == o.str_val;
    return to_float() == o.to_float();
}
bool Value::operator<(const Value& o)  const {
    if (type_ == STRING && o.type_ == STRING) return str_val < o.str_val;
    return to_float() < o.to_float();
}
bool Value::operator<=(const Value& o) const {
    if (type_ == STRING && o.type_ == STRING) return str_val <= o.str_val;
    return to_float() <= o.to_float();
}
bool Value::operator>(const Value& o)  const {
    if (type_ == STRING && o.type_ == STRING) return str_val > o.str_val;
    return to_float() > o.to_float();
}
bool Value::operator>=(const Value& o) const {
    if (type_ == STRING && o.type_ == STRING) return str_val >= o.str_val;
    return to_float() >= o.to_float();
}
int  Value::spaceship(const Value& o)  const { return (*this == o) ? 0 : (*this < o ? -1 : 1); }

Value Value::bitwise_and(const Value& o) const { return Value(to_int() & o.to_int()); }
Value Value::bitwise_or(const Value& o)  const { return Value(to_int() | o.to_int()); }
Value Value::bitwise_xor(const Value& o) const { return Value(to_int() ^ o.to_int()); }
Value Value::bitwise_not()               const { return Value(~to_int()); }
Value Value::shift_left(const Value& o)  const { return Value(to_int() << o.to_int()); }
Value Value::shift_right(const Value& o) const { return Value(to_int() >> o.to_int()); }

// â”€â”€ Interpreter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static void init_core_modules(std::map<std::string, Module>& stdlib,
                               std::map<std::string, Module>& modules) {
    // log:: is a core module — accessible via :: without 'use log;'
    if (stdlib.count("log"))  modules["log"]  = stdlib["log"];
    if (stdlib.count("file")) modules["file"] = stdlib["file"];
    if (stdlib.count("date")) modules["date"] = stdlib["date"];
}

Interpreter::Interpreter() {
    globals_ = std::make_shared<Environment>();
    current_ = globals_;
    output_stream_ = &std::cout;
    stdlib_ = make_stdlib();
    auto extra = make_extra_stdlib(this);
    for (auto& [k,v] : extra) stdlib_[k] = std::move(v);
    init_core_modules(stdlib_, modules_);
}

Interpreter::Interpreter(std::ostream& out) {
    globals_ = std::make_shared<Environment>();
    current_ = globals_;
    output_stream_ = &out;
    stdlib_ = make_stdlib();
    auto extra = make_extra_stdlib(this);
    for (auto& [k,v] : extra) stdlib_[k] = std::move(v);
    init_core_modules(stdlib_, modules_);
}

std::vector<std::string> Interpreter::get_global_names() const {
    std::vector<std::string> names;
    for (auto& [k, v] : globals_->entries()) {
        if (k.size() >= 2 && k[0] == '_' && k[1] == '_') continue;  // skip __assoc__ etc.
        if (v.type() == Value::FUNCTION || v.type() == Value::BYTECODE_FN) continue; // skip functions for :vars
        names.push_back(k);
    }
    return names;
}

void Interpreter::register_builtin(const std::string& name, std::function<Value(std::vector<Value>)> fn) {
    // Wrap as a NativeFn and store in globals as a callable Value via a LookFunction shim
    // Simplest: store directly in a custom map, handled in call_function
    // Actually: wrap in Module with name "" and one function — simpler to add to a fake module
    // Easiest correct path: store in globals_ as a native-fn Value through a LookFunction wrapper
    // We use a Module trick: add a single-function module named "" and lookup by name
    // Correct approach: add to a "builtins_" map checked in call-by-name resolution
    builtins_[name] = std::move(fn);
}

// ── LookChannel methods ───────────────────────────────────────────────────────

void LookChannel::send_val(Value val) {
    std::unique_lock<std::mutex> lk(mtx);
    not_full.wait(lk, [this]{ return closed || queue.size() < capacity; });
    if (closed) throw std::runtime_error("send on closed channel");
    queue.push(std::move(val));
    not_empty.notify_one();
}

Value LookChannel::recv_val() {
    std::unique_lock<std::mutex> lk(mtx);
    not_empty.wait(lk, [this]{ return closed || !queue.empty(); });
    if (queue.empty()) return Value();  // closed + empty → null
    Value v = std::move(queue.front());
    queue.pop();
    not_full.notify_one();
    return v;
}

void LookChannel::close_chan() {
    std::unique_lock<std::mutex> lk(mtx);
    closed = true;
    not_empty.notify_all();
    not_full.notify_all();
}

// ── Thread-safe dispatch copy ─────────────────────────────────────────────────
// Shares read-only setup state (route_registry, struct_defs, globals).
// Each copy has its own output, web_ctx, call_depth, call_stack, current env.
// Caller must call set_output() and set_web_context() before dispatch_routes().
std::unique_ptr<Interpreter> Interpreter::make_dispatch_copy() const {
    auto c = std::make_unique<Interpreter>();   // initialises stdlib_ + fresh globals_
    c->globals_        = globals_->clone();    // snapshot — her dispatch kendi globals_ kopyasına yazar
    c->current_        = std::make_shared<Environment>(c->globals_);  // fresh dispatch scope
    c->route_registry_ = route_registry_;       // copy (closures are read-only)
    c->struct_defs_    = struct_defs_;          // copy (user struct definitions)
    c->modules_        = modules_;              // copy (use X state from setup)
    c->setup_mode_     = false;
    c->main_script_    = main_script_;
    c->current_file_   = current_file_;
    c->start_time_     = start_time_;
    c->builtins_       = builtins_;      // test runner & extension registrations
    return c;
}

// â”€â”€ .env loader â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::map<std::string, std::string> g_env_vars;
static std::map<std::string, std::map<std::string, Value>> g_config;
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

    // APP_ENV'e gore log seviyesi ayarla
    auto env_it = g_env_vars.find("APP_ENV");
    std::string app_env = (env_it != g_env_vars.end()) ? env_it->second : "production";

    auto debug_it = g_env_vars.find("APP_DEBUG");
    bool debug = (debug_it != g_env_vars.end() && debug_it->second == "true");

    auto log_dir_it = g_env_vars.find("LOG_DIR");
    std::string log_dir = (log_dir_it != g_env_vars.end()) ? log_dir_it->second : "logs";
    // Path traversal koruması: ../ ve mutlak path reddedilir
    if (log_dir.find("..") != std::string::npos ||
        (!log_dir.empty() && (log_dir[0] == '/' || log_dir[0] == '\\')))
        log_dir = "logs";

    bool verbose    = (app_env == "development") || debug;
    LogLevel level  = (app_env == "development" || debug) ? LogLevel::LOG_DEBUG : LogLevel::LOG_INFO;

    Logger::instance().configure(log_dir, verbose, level);
}

void Interpreter::set_web_context(WebContext* ctx) {
    web_ctx_ = ctx;
    // Web core modülleri: stdlib_'e VE modules_'a ekle
    // use gerekmez — db::, request::, response:: vb. doğrudan erişilebilir
    if (ctx) {
        auto web_mods = make_web_modules(ctx);
        for (auto& [name, mod] : web_mods) {
            stdlib_[name]  = mod;
            modules_[name] = std::move(mod);
        }
    }
}

NativeFn Interpreter::get_module_fn(const std::string& module_name, const std::string& fn_name) const {
    auto mit = modules_.find(module_name);
    if (mit == modules_.end()) return nullptr;
    auto fit = mit->second.functions.find(fn_name);
    if (fit == mit->second.functions.end()) return nullptr;
    return fit->second;
}

void Interpreter::interpret(const Program& program) {
    try {
        for (const auto& stmt : program.statements)
            execute_statement(*stmt);
    } catch (const LookRuntimeError&) {
        throw;  // already enriched — pass through
    } catch (const std::runtime_error& e) {
        // Enrich plain runtime_error with location + stack
        throw LookRuntimeError(e.what(), current_loc_, call_stack_);
    }
}

// â”€â”€ String interpolation with full expression support â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Supports: {$var}  {$a + $b}
// Only {$ triggers interpolation — {#...} and other {non-$} are literal.
// This matches compiler (VM) behavior and prevents conflicts with template syntax.

std::string Interpreter::interpolate_string(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());
    size_t i = 0;

    while (i < raw.size()) {
        // Interpolation: {$var}, {true}, {null}, {identifier}, {module::func()}
        // Trigger: { followed by $, letter, or underscore — NOT {# (template directive)
        bool is_interp = false;
        if (raw[i] == '{' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            is_interp = (next == '$' || std::isalpha((unsigned char)next) || next == '_');
        }
        if (is_interp) {
            // Find matching closing brace (respects nesting)
            size_t depth = 1;
            size_t j = i + 1;
            while (j < raw.size() && depth > 0) {
                if (raw[j] == '{') depth++;
                else if (raw[j] == '}') depth--;
                if (depth > 0) j++;
            }
            if (depth != 0) { result += raw[i++]; continue; }

            std::string expr_src = raw.substr(i + 1, j - i - 1);
            try {
                // Parse and evaluate the expression
                Lexer   lex(expr_src + ";");
                Parser  par(lex.scan_tokens());
                auto    prog = par.parse();
                if (!prog->statements.empty()) {
                    if (auto* es = dynamic_cast<ExpressionStatement*>(prog->statements[0].get())) {
                        result += evaluate_expression(*es->expression).to_string();
                    }
                }
            } catch (...) {
                result += raw.substr(i, j - i + 1);
            }
            i = j + 1;
        } else {
            result += raw[i++];
        }
    }
    return result;
}

// â”€â”€ print / write output helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::string Interpreter::build_output(const std::vector<std::unique_ptr<Expression>>& exprs) {
    std::string out;
    for (size_t i = 0; i < exprs.size(); ++i) {
        if (i > 0) out += " ";
        out += evaluate_expression(*exprs[i]).to_string();
    }
    return out;
}

// â”€â”€ execute_statement â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void Interpreter::execute_statement(const Statement& stmt) {
    // Track statement location
    if (stmt.loc.line > 0) current_loc_ = stmt.loc;

    if (auto* s = dynamic_cast<const UseStatement*>(&stmt)) {
        auto it = stdlib_.find(s->module_name);
        if (it != stdlib_.end()) {
            std::string key = s->alias.empty() ? s->module_name : s->alias;
            modules_[key] = it->second;
            return;
        }

        // stdlib'de yok — ~/.look/modules/<name>/<name>.lk dosyasına bak
        {
            interp_fs::path module_file;
#ifdef _WIN32
            const char* home = std::getenv("USERPROFILE");
            if (!home) home = std::getenv("HOMEDRIVE");
#else
            const char* home = std::getenv("HOME");
#endif
            if (home) {
                module_file = interp_fs::path(home) / ".look" / "modules"
                            / s->module_name / (s->module_name + ".lk");
            }

            if (!module_file.empty() && interp_fs::exists(module_file)) {
                std::string abs_path = module_file.string();

                if (included_files_.count(abs_path)) return; // zaten yüklendi
                included_files_.insert(abs_path);

                std::ifstream f(abs_path);
                if (!f) throw std::runtime_error("Modül dosyası açılamadı: " + abs_path);
                std::string src((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

                Lexer lx(src);
                auto toks = lx.scan_tokens();
                Parser p(std::move(toks));
                auto prog = p.parse();

                auto prev_file = current_file_;
                current_file_ = abs_path;
                auto prev_env  = current_;
                auto isolated  = std::make_shared<Environment>(globals_);
                current_ = isolated;
                try {
                    for (auto& sub : prog->statements) execute_statement(*sub);
                } catch (...) {
                    current_ = prev_env; current_file_ = prev_file; throw;
                }
                current_ = prev_env;
                current_file_ = prev_file;
                for (auto& [name, val] : isolated->entries()) {
                    if (name.empty() || name[0] == '$') continue;
                    globals_->define(name, val);
                }
                owned_programs_.push_back(std::move(prog));
                return;
            }
        }

        throw std::runtime_error("Unknown module: '" + s->module_name
            + "'. Kurmak için: lk module install " + s->module_name);
    }

    // Phase 18.5 — dosya modül sistemi
    if (auto* s = dynamic_cast<const UseFileStatement*>(&stmt)) {
        // 1. Mutlak path hesapla — mevcut dosyanın dizinine göre
        interp_fs::path base;
        if (!current_file_.empty())
            base = interp_fs::path(current_file_).parent_path();
        else
            base = interp_fs::current_path();

        interp_fs::path target = interp_fs::weakly_canonical(base / s->path);
        std::string abs_path = target.string();

        // 1b. Project root sınırı — ana scriptin dizini dışına çıkış yasak
        {
            interp_fs::path root;
            if (!main_script_.empty())
                root = interp_fs::weakly_canonical(interp_fs::path(main_script_).parent_path());
            else
                root = interp_fs::weakly_canonical(interp_fs::current_path());
            std::string root_str = root.string();
            if (abs_path.substr(0, root_str.size()) != root_str)
                throw LookRuntimeError(
                    "use \"" + s->path + "\": dosya modülü proje dizini dışına çıkamaz",
                    stmt.loc, call_stack_);
        }

        // 2. Döngüsel include koruması
        if (included_files_.count(abs_path))
            throw LookRuntimeError(
                "Circular include: \"" + s->path + "\"", stmt.loc, call_stack_);
        included_files_.insert(abs_path);

        // 3. Dosyayı oku
        std::ifstream f(abs_path);
        if (!f)
            throw LookRuntimeError(
                "Cannot open included file: \"" + s->path + "\"", stmt.loc, call_stack_);
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

        // 4. Lex + parse
        Lexer lx(src);
        std::vector<Token> toks;
        try { toks = lx.scan_tokens(); }
        catch (const std::exception& e) {
            throw LookRuntimeError(
                "Lexer error in \"" + s->path + "\": " + e.what(), stmt.loc, call_stack_);
        }
        Parser p(std::move(toks));
        std::unique_ptr<Program> prog;
        try { prog = p.parse(); }
        catch (const LookParseError& e) {
            throw LookParseError(e.message, e.line, e.column, abs_path);
        }

        // 5. İzole ortamda çalıştır — $var tanımları sızmaz
        auto prev_file = current_file_;
        current_file_ = abs_path;
        auto prev_env  = current_;
        auto isolated  = std::make_shared<Environment>(globals_);
        current_ = isolated;

        try {
            for (auto& sub : prog->statements)
                execute_statement(*sub);
        } catch (...) {
            current_ = prev_env;
            current_file_ = prev_file;
            throw;
        }

        current_ = prev_env;
        current_file_ = prev_file;

        // 6. Sadece function ve const ($ içermeyen isimler) globals_'a aktar
        for (auto& [name, val] : isolated->entries()) {
            if (name.empty() || name[0] == '$') continue;  // $var — sızmasın
            globals_->define(name, val);
        }

        // 7. AST'yi sakla — LookFunction::body pointer'ları bu AST'e bakıyor
        owned_programs_.push_back(std::move(prog));
        return;
    }
    if (auto* s = dynamic_cast<const PrintStatement*>(&stmt)) {
        *output_stream_ << build_output(s->expressions) << "\n";
        return;
    }
    if (auto* s = dynamic_cast<const WriteStatement*>(&stmt)) {
        *output_stream_ << build_output(s->expressions);
        return;
    }
    if (auto* s = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        Value v = evaluate_expression(*s->expression);
        if (repl_value_cb_) repl_value_cb_(v);
        return;
    }
    if (auto* s = dynamic_cast<const BlockStatement*>(&stmt)) {
        execute_block(*s, current_);
        return;
    }
    if (auto* s = dynamic_cast<const IfStatement*>(&stmt)) {
        if (evaluate_expression(*s->condition).is_truthy())
            execute_block(*s->then_branch, current_);
        else if (s->else_branch)
            execute_block(*s->else_branch, current_);
        return;
    }
    if (auto* s = dynamic_cast<const WhileStatement*>(&stmt)) {
        while (evaluate_expression(*s->condition).is_truthy()) {
            try { execute_block(*s->body, current_); }
            catch (const BreakException&)    { break; }
            catch (const ContinueException&) { continue; }
        }
        return;
    }
    if (auto* s = dynamic_cast<const ForStatement*>(&stmt)) {
        auto scope = std::make_shared<Environment>(current_);
        auto prev  = current_;
        current_   = scope;
        if (s->init) execute_statement(*s->init);
        while (!s->condition || evaluate_expression(*s->condition).is_truthy()) {
            try { execute_block(*s->body, current_); }
            catch (const BreakException&)    { break; }
            catch (const ContinueException&) {}
            if (s->post) evaluate_expression(*s->post);
        }
        current_ = prev;
        return;
    }
    if (auto* s = dynamic_cast<const ForeachStatement*>(&stmt)) {
        Value iterable = evaluate_expression(*s->iterable);
        if (iterable.type() != Value::ARRAY)
            throw std::runtime_error("foreach requires an array");
        auto& arr = *iterable.as_array();

        // Assoc array: ["__assoc__", k0, v0, k1, v1, ...]
        bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING &&
                        arr[0].as_string() == "__assoc__";

        if (is_assoc) {
            for (size_t idx = 1; idx + 1 < arr.size(); idx += 2) {
                auto env  = std::make_shared<Environment>(current_);
                auto prev = current_;
                current_  = env;
                if (!s->key_var.empty()) env->define(s->key_var, arr[idx]);
                env->define(s->value_var, arr[idx + 1]);
                try { for (const auto& st : s->body->statements) execute_statement(*st); }
                catch (const BreakException&)    { current_ = prev; break; }
                catch (const ContinueException&) { current_ = prev; continue; }
                catch (...) { current_ = prev; throw; }
                current_ = prev;
            }
            return;
        }

        for (size_t idx = 0; idx < arr.size(); ++idx) {
            auto env  = std::make_shared<Environment>(current_);
            auto prev = current_;
            current_  = env;
            if (!s->key_var.empty())
                env->define(s->key_var, Value((int)idx));
            env->define(s->value_var, arr[idx]);
            try { for (const auto& st : s->body->statements) execute_statement(*st); }
            catch (const BreakException&)    { current_ = prev; break; }
            catch (const ContinueException&) { current_ = prev; continue; }
            catch (...) { current_ = prev; throw; }
            current_ = prev;
        }
        return;
    }
    if (auto* s = dynamic_cast<const TryCatchStatement*>(&stmt)) {
        bool caught = false;
        try {
            execute_block(*s->try_block, current_);
        } catch (const ReturnException&) {
            if (s->finally_block) execute_block(*s->finally_block, current_);
            throw;
        } catch (const BreakException&) {
            if (s->finally_block) execute_block(*s->finally_block, current_);
            throw;
        } catch (const ContinueException&) {
            if (s->finally_block) execute_block(*s->finally_block, current_);
            throw;
        } catch (const ExitException&) {
            // exit() / die() — finally çalıştır ama catch'e düşme, yukarı ilet
            if (s->finally_block) execute_block(*s->finally_block, current_);
            throw;
        } catch (const RouteMatchedException&) {
            // route() flow control — catch'e düşmemeli, yukarı ilet
            if (s->finally_block) execute_block(*s->finally_block, current_);
            throw;
        } catch (const std::exception& e) {
            caught = true;
            if (s->catch_block) {
                if (!s->catch_var.empty())
                    current_->define(s->catch_var, Value(std::string(e.what())));
                execute_block(*s->catch_block, current_);
            }
        }
        if (s->finally_block) execute_block(*s->finally_block, current_);
        return;
    }
    if (auto* s = dynamic_cast<const SwitchStatement*>(&stmt)) {
        Value subject = evaluate_expression(*s->subject);
        const SwitchCase* default_case = nullptr;
        bool matched = false;
        auto run_case = [&](const SwitchCase& sc) {
            auto env = std::make_shared<Environment>(current_);
            auto prev = current_;
            current_ = env;
            try {
                for (const auto& body_stmt : sc.body)
                    execute_statement(*body_stmt);
            } catch (...) { current_ = prev; throw; }
            current_ = prev;
        };
        for (const auto& sc : s->cases) {
            if (sc.values.empty()) { default_case = &sc; continue; }
            for (const auto& val_expr : sc.values) {
                if (subject == evaluate_expression(*val_expr)) {
                    run_case(sc);
                    matched = true;
                    break;
                }
            }
            if (matched) break;
        }
        if (!matched && default_case)
            run_case(*default_case);
        return;
    }
    if (dynamic_cast<const BreakStatement*>(&stmt))    { throw BreakException(); }
    if (dynamic_cast<const ContinueStatement*>(&stmt)) { throw ContinueException(); }
    if (auto* s = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        auto fn = std::make_shared<LookFunction>(s->name, s->parameters, s->is_variadic, s->body.get(), current_);
        current_->define(s->name, Value(fn));
        return;
    }
    // Phase 11: struct declaration
    if (auto* s = dynamic_cast<const StructDeclaration*>(&stmt)) {
        std::vector<StructFieldDef> defs;
        for (const auto& f : s->fields) {
            StructFieldDef sfd;
            sfd.name = f.name;
            if (f.default_expr) {
                sfd.has_default = true;
                sfd.default_val = evaluate_expression(*f.default_expr);
            }
            defs.push_back(std::move(sfd));
        }
        struct_defs_[s->name] = std::move(defs);
        return;
    }
    // Phase 11: const block
    if (auto* s = dynamic_cast<const ConstBlock*>(&stmt)) {
        const Expression* last_expr = nullptr;
        for (int i = 0; i < (int)s->items.size(); i++) {
            const auto& item = s->items[i];
            current_iota_ = i;
            Value val;
            if (item.value) {
                last_expr = item.value.get();
                val = evaluate_expression(*last_expr);
            } else if (last_expr) {
                // Go-style: re-evaluate previous expression with updated iota
                val = evaluate_expression(*last_expr);
            } else {
                val = Value(i);
            }
            try { current_->set(item.name, val); } catch (...) { current_->define(item.name, val); }
        }
        current_iota_ = 0;
        return;
    }
    if (auto* s = dynamic_cast<const ReturnStatement*>(&stmt)) {
        Value v = s->expression ? evaluate_expression(*s->expression) : Value();
        throw ReturnException(v);
    }
    throw std::runtime_error("Unknown statement type");
}

void Interpreter::execute_block(const BlockStatement& block, std::shared_ptr<Environment> enclosing) {
    auto env  = std::make_shared<Environment>(enclosing);
    auto prev = current_;
    current_  = env;
    try {
        for (const auto& s : block.statements)
            execute_statement(*s);
    } catch (...) {
        current_ = prev;
        throw;
    }
    current_ = prev;
}

// â”€â”€ evaluate_expression â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

Value Interpreter::evaluate_expression(const Expression& expr) {
    // Track current source location for error reporting
    if (expr.loc.line > 0) current_loc_ = expr.loc;

    // Module scope resolution
    if (auto* e = dynamic_cast<const ScopeResolution*>(&expr)) {
        auto mod_it = modules_.find(e->module_name);
        if (mod_it == modules_.end())
            throw std::runtime_error("Module '" + e->module_name + "' not loaded. Add: use " + e->module_name + ";");
        auto fn_it = mod_it->second.functions.find(e->member_name);
        if (fn_it == mod_it->second.functions.end())
            throw std::runtime_error("'" + e->module_name + "' has no function '" + e->member_name + "'");
        return Value(std::string("__module__:" + e->module_name + "::" + e->member_name));
    }

    // Phase 11: iota — current counter value set by ConstBlock evaluator
    if (dynamic_cast<const IotaExpression*>(&expr)) {
        return Value(current_iota_);
    }

    // Phase 11: $obj.field — member access on struct/assoc array
    if (auto* e = dynamic_cast<const MemberAccessExpression*>(&expr)) {
        Value obj = evaluate_expression(*e->object);
        if (obj.type() != Value::ARRAY)
            throw LookRuntimeError("Member access '." + e->field + "' requires a struct or assoc array", current_loc_);
        auto& arr = *obj.as_array();
        if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__") {
            for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                if (arr[i].to_string() == e->field) return arr[i + 1];
            }
            return Value(); // null if field not found
        }
        throw LookRuntimeError("Cannot access field '" + e->field + "' on a non-struct value", current_loc_);
    }

    // Phase 11: Struct literal — Kullanici{ad: "Ali", yas: 30}
    if (auto* e = dynamic_cast<const StructLiteralExpression*>(&expr)) {
        auto it = struct_defs_.find(e->struct_name);
        if (it == struct_defs_.end())
            throw LookRuntimeError("Unknown struct '" + e->struct_name + "'", current_loc_);
        const auto& defs = it->second;

        // Validate: no unknown fields in literal
        for (const auto& kv : e->fields) {
            bool found = false;
            for (const auto& def : defs) { if (def.name == kv.first) { found = true; break; } }
            if (!found)
                throw LookRuntimeError("Unknown field '" + kv.first + "' in struct '" + e->struct_name + "'", current_loc_);
        }

        // Build assoc array: __assoc__ sentinel + __struct__ tag + all fields in declaration order
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        arr->push_back(Value(std::string("__struct__")));
        arr->push_back(Value(e->struct_name));

        for (const auto& def : defs) {
            arr->push_back(Value(def.name));
            bool found = false;
            for (const auto& kv : e->fields) {
                if (kv.first == def.name) {
                    arr->push_back(evaluate_expression(*kv.second));
                    found = true;
                    break;
                }
            }
            if (!found)
                arr->push_back(def.has_default ? def.default_val : Value());
        }
        return Value(arr);
    }

    // Ternary: $cond ? $then : $else
    if (auto* e = dynamic_cast<const TernaryExpression*>(&expr)) {
        return evaluate_expression(*e->condition).is_truthy()
            ? evaluate_expression(*e->then_expr)
            : evaluate_expression(*e->else_expr);
    }

    // Literals
    if (auto* e = dynamic_cast<const NumberLiteral*>(&expr))  return Value(e->value);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))   return Value(e->value);
    if (auto* e = dynamic_cast<const BooleanLiteral*>(&expr)) return Value(e->value);
    if (dynamic_cast<const NullLiteral*>(&expr))              return Value();

    if (auto* e = dynamic_cast<const StringLiteral*>(&expr))
        return Value(interpolate_string(e->value));

    // Anonymous function expression â†’ Value(LookFunction)
    if (auto* e = dynamic_cast<const FunctionExpression*>(&expr)) {
        // use ($conn, $db) â€” captured variables'i closure env'e inject et
        auto closure_env = current_;
        if (!e->captures.empty()) {
            auto captured = std::make_shared<Environment>(globals_);
            for (const auto& name : e->captures) {
                try {
                    captured->define(name, current_->get(name));
                } catch (...) {
                    // Degisken bulunamazsa null ile devam et
                    captured->define(name, Value());
                }
            }
            closure_env = captured;
        }
        auto fn = std::make_shared<LookFunction>("__anonymous__", e->parameters, e->is_variadic, e->body.get(), closure_env);
        return Value(fn);
    }

    // Array literal [1, 2, 3]
    if (auto* e = dynamic_cast<const ArrayLiteral*>(&expr)) {
        auto arr = std::make_shared<std::vector<Value>>();
        for (const auto& el : e->elements)
            arr->push_back(evaluate_expression(*el));
        return Value(arr);
    }

    // Associative array ["key" => val] â†’ stored as flat [k, v, k, v, ...] pairs
    // Accessible via assoc::get($arr, "key") or arr[0] for iteration
    if (auto* e = dynamic_cast<const AssocArrayLiteral*>(&expr)) {
        auto arr = std::make_shared<std::vector<Value>>();
        for (const auto& pair : e->pairs) {
            arr->push_back(evaluate_expression(*pair.first));
            arr->push_back(evaluate_expression(*pair.second));
        }
        // Tag as assoc with sentinel at position 0
        // Actually: store as interleaved [k0, v0, k1, v1, ...]
        // db::col and foreach will work on the raw array
        // We mark it with a special first element
        auto result = std::make_shared<std::vector<Value>>();
        result->push_back(Value(std::string("__assoc__")));
        result->insert(result->end(), arr->begin(), arr->end());
        return Value(result);
    }

    // Variable
    if (auto* e = dynamic_cast<const Variable*>(&expr))
        return current_->get(e->name);

    // Index access $arr[i] or $assoc["key"]
    if (auto* e = dynamic_cast<const IndexExpression*>(&expr)) {
        Value obj = evaluate_expression(*e->object);
        Value idx = evaluate_expression(*e->index);
        if (obj.type() != Value::ARRAY)
            throw std::runtime_error("Index operator requires an array");
        auto& arr = *obj.as_array();

        // Associative array: string key access
        if (!arr.empty() && arr[0].type() == Value::STRING &&
            arr[0].as_string() == "__assoc__" && idx.type() == Value::STRING) {
            const std::string& key = idx.as_string();
            for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                if (arr[i].to_string() == key) return arr[i + 1];
            }
            return Value(); // null if not found
        }

        // Regular numeric index
        int i = idx.to_int();
        if (i < 0) i = (int)arr.size() + i;
        if (i < 0 || i >= (int)arr.size())
            throw std::runtime_error("Array index " + std::to_string(i) + " out of bounds");
        return arr[i];
    }

    // Assignment
    if (auto* e = dynamic_cast<const AssignmentExpression*>(&expr)) {
        Value val = evaluate_expression(*e->value);

        // $arr[i] = val  /  $assoc["key"] = val
        if (e->index) {
            Value obj = current_->get(e->name);
            if (obj.type() != Value::ARRAY)
                throw std::runtime_error(e->name + " is not an array");
            Value idx = evaluate_expression(*e->index);
            auto& arr = *obj.as_array();

            // Assoc array: string key
            if (!arr.empty() && arr[0].type() == Value::STRING &&
                arr[0].as_string() == "__assoc__" && idx.type() == Value::STRING) {
                const std::string& key = idx.as_string();
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    if (arr[i].to_string() == key) { arr[i + 1] = val; return val; }
                }
                // Key yok — yeni key/value ekle
                arr.push_back(Value(key));
                arr.push_back(val);
                return val;
            }

            // Numeric index
            int i = idx.to_int();
            if (i < 0) i = (int)arr.size() + i;
            if (i == (int)arr.size()) arr.push_back(val);
            else if (i >= 0 && i < (int)arr.size()) arr[i] = val;
            else throw std::runtime_error("Array index out of bounds");
            return val;
        }

        // $var op= val
        if (e->op != "=") {
            Value cur = current_->get(e->name);
            const std::string& op = e->op;
            if      (op == "+=")  val = cur + val;
            else if (op == "-=")  val = cur - val;
            else if (op == "*=")  val = cur * val;
            else if (op == "/=")  val = cur / val;
            else if (op == "%=")  val = cur % val;
            else if (op == ".=")  val = cur.concat(val);
            else if (op == "&=")  val = cur.bitwise_and(val);
            else if (op == "|=")  val = cur.bitwise_or(val);
            else if (op == "^=")  val = cur.bitwise_xor(val);
        }

        try { current_->set(e->name, val); }
        catch (...) { current_->define(e->name, val); }
        return val;
    }

    // Unary
    if (auto* e = dynamic_cast<const UnaryExpression*>(&expr)) {
        const std::string& op = e->op;
        if (op == "++" || op == "--") {
            auto* var = dynamic_cast<const Variable*>(e->right.get());
            if (!var) throw std::runtime_error("++/-- requires a variable");
            Value cur  = current_->get(var->name);
            Value next = (op == "++") ? cur + Value(1) : cur - Value(1);
            try { current_->set(var->name, next); } catch(...) { current_->define(var->name, next); }
            return e->prefix ? next : cur;
        }
        Value right = evaluate_expression(*e->right);
        if (op == "-") { if (right.type()==Value::FLOAT) return Value(-right.as_float()); return Value(-right.to_int()); }
        if (op == "!") return Value(!right.is_truthy());
        if (op == "~") return right.bitwise_not();
        throw std::runtime_error("Unknown unary op: " + op);
    }

    // Binary
    if (auto* e = dynamic_cast<const BinaryExpression*>(&expr)) {
        const std::string& op = e->op;
        if (op == "&&") { Value l = evaluate_expression(*e->left); if (!l.is_truthy()) return Value(false); return Value(evaluate_expression(*e->right).is_truthy()); }
        if (op == "||") { Value l = evaluate_expression(*e->left); if (l.is_truthy())  return Value(true);  return Value(evaluate_expression(*e->right).is_truthy()); }
        if (op == "??") { Value l = evaluate_expression(*e->left); return (l.type() != Value::NONE) ? l : evaluate_expression(*e->right); }

        Value left  = evaluate_expression(*e->left);
        Value right = evaluate_expression(*e->right);

        if (op == "+")   return left + right;
        if (op == "-")   return left - right;
        if (op == "*")   return left * right;
        if (op == "/")   return left / right;
        if (op == "%")   return left % right;
        if (op == "**")  return left.pow(right);
        if (op == ".")   return left.concat(right);
        if (op == "==")  return Value(left == right);
        if (op == "!=")  return Value(!(left == right));
        if (op == "<")   return Value(left < right);
        if (op == "<=")  return Value(left <= right);
        if (op == ">")   return Value(left > right);
        if (op == ">=")  return Value(left >= right);
        if (op == "<=>") return Value(left.spaceship(right));
        if (op == "&")   return left.bitwise_and(right);
        if (op == "|")   return left.bitwise_or(right);
        if (op == "^")   return left.bitwise_xor(right);
        if (op == "<<")  return left.shift_left(right);
        if (op == ">>")  return left.shift_right(right);

        throw std::runtime_error("Unknown binary op: " + op);
    }

    // Function call
    if (auto* e = dynamic_cast<const CallExpression*>(&expr)) {
        std::string fn_name;
        if (auto* var = dynamic_cast<const Variable*>(e->callee.get()))
            fn_name = var->name;

        // Module call: math::sqrt(x)
        if (auto* sr = dynamic_cast<const ScopeResolution*>(e->callee.get())) {

            // ── ws:: — Phase 15 WebSocket ─────────────────────────────────────
            if (sr->module_name == "ws") {
                const std::string& fn = sr->member_name;
                auto argc_ws = e->arguments.size();

                // ws::on($ws, event, fn)
                if (fn == "on") {
                    if (argc_ws != 3) throw std::runtime_error("ws::on() takes 3 arguments");
                    Value ws_v  = evaluate_expression(*e->arguments[0]);
                    std::string ev = evaluate_expression(*e->arguments[1]).to_string();
                    Value cb_v  = evaluate_expression(*e->arguments[2]);
                    if (ws_v.type() != Value::WEBSOCKET || !ws_v.as_websocket())
                        throw std::runtime_error("ws::on() first argument must be a websocket");
                    if (cb_v.type() != Value::FUNCTION)
                        throw std::runtime_error("ws::on() third argument must be a function");
                    auto conn = ws_v.as_websocket();
                    // Create a base interpreter copy — each event invocation gets its own copy.
                    auto base = std::shared_ptr<Interpreter>(make_dispatch_copy().release());
                    if (ev == "message") {
                        conn->on_message = [base, cb_v](const std::string& msg) {
                            auto copy = base->make_dispatch_copy();
                            std::ostringstream out; copy->set_output(out);
                            look::acquire_thread_connections();
                            try { copy->invoke(cb_v, {Value(msg)}); } catch (...) {}
                            look::release_thread_connections();
                        };
                    } else if (ev == "close") {
                        conn->on_close = [base, cb_v]() {
                            auto copy = base->make_dispatch_copy();
                            std::ostringstream out; copy->set_output(out);
                            look::acquire_thread_connections();
                            try { copy->invoke(cb_v, {}); } catch (...) {}
                            look::release_thread_connections();
                        };
                    }
                    return Value();
                }

                // ws::send($ws, msg)
                if (fn == "send") {
                    if (argc_ws != 2) throw std::runtime_error("ws::send() takes 2 arguments");
                    Value ws_v = evaluate_expression(*e->arguments[0]);
                    std::string msg = evaluate_expression(*e->arguments[1]).to_string();
                    if (ws_v.type() != Value::WEBSOCKET || !ws_v.as_websocket())
                        throw std::runtime_error("ws::send() first argument must be a websocket");
                    return Value(ws_v.as_websocket()->send_text(msg));
                }

                // ws::close($ws)
                if (fn == "close") {
                    if (argc_ws != 1) throw std::runtime_error("ws::close() takes 1 argument");
                    Value ws_v = evaluate_expression(*e->arguments[0]);
                    if (ws_v.type() != Value::WEBSOCKET || !ws_v.as_websocket())
                        throw std::runtime_error("ws::close() argument must be a websocket");
                    ws_v.as_websocket()->close_conn();
                    return Value();
                }

                // ws::broadcast(msg)
                if (fn == "broadcast") {
                    if (argc_ws != 1) throw std::runtime_error("ws::broadcast() takes 1 argument");
                    std::string msg = evaluate_expression(*e->arguments[0]).to_string();
                    look::g_ws_registry.broadcast(msg);
                    return Value();
                }

                // ws::clients() → count of connected clients
                if (fn == "clients") {
                    return Value((int)look::g_ws_registry.count());
                }

                throw std::runtime_error("ws::" + fn + "() not found");
            }

            // ── timer:: — Phase 16 ────────────────────────────────────────────
            if (sr->module_name == "timer") {
                const std::string& fn = sr->member_name;
                auto argc_t = e->arguments.size();

                // timer::after(ms, fn) / timer::every(ms, fn) → returns timer id
                if (fn == "after" || fn == "every") {
                    if (argc_t < 2) throw std::runtime_error("timer::" + fn + "() takes 2 arguments");
                    int ms = evaluate_expression(*e->arguments[0]).to_int();
                    Value cb_v = evaluate_expression(*e->arguments[1]);
                    if (cb_v.type() != Value::FUNCTION)
                        throw std::runtime_error("timer::" + fn + "() second argument must be a function");

                    auto base = std::shared_ptr<Interpreter>(make_dispatch_copy().release());
                    auto sink = std::make_shared<std::ostringstream>();

                    auto callback = [base, sink, cb_v]() mutable {
                        look::WebContext ctx;
                        ctx.method = "__TIMER__";
                        sink->str(""); sink->clear();
                        base->set_output(*sink);
                        base->set_web_context(&ctx);
                        look::acquire_thread_connections();
                        try {
                            base->invoke(cb_v, {});
                        } catch (const std::exception& ex) {
                            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "timer",
                                std::string("callback hata: ") + ex.what());
                        } catch (...) {
                            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "timer",
                                "callback bilinmeyen hata");
                        }
                        look::release_thread_connections();
                    };

                    int id;
                    if (fn == "after")
                        id = look::TimerManager::instance().after(ms, std::move(callback));
                    else
                        id = look::TimerManager::instance().every(ms, std::move(callback));
                    return Value(id);
                }

                // timer::cancel($id)
                if (fn == "cancel") {
                    if (argc_t < 1) throw std::runtime_error("timer::cancel() takes 1 argument");
                    int id = evaluate_expression(*e->arguments[0]).to_int();
                    look::TimerManager::instance().cancel(id);
                    return Value();
                }

                throw std::runtime_error("timer::" + fn + "() not found");
            }

            // ── sse:: — Phase 16 Server-Sent Events ──────────────────────────
            if (sr->module_name == "sse") {
                const std::string& fn = sr->member_name;
                auto argc_s = e->arguments.size();

                // sse::send($sse, $data [, "event-name"])
                if (fn == "send") {
                    if (argc_s < 2) throw std::runtime_error("sse::send() takes at least 2 arguments");
                    Value sse_v = evaluate_expression(*e->arguments[0]);
                    std::string data = evaluate_expression(*e->arguments[1]).to_string();
                    std::string ev_name = "";
                    if (argc_s >= 3) ev_name = evaluate_expression(*e->arguments[2]).to_string();
                    if (sse_v.type() != Value::SSE_CONN || !sse_v.as_sse())
                        throw std::runtime_error("sse::send() first argument must be an SSE connection");
                    return Value(sse_v.as_sse()->send(data, ev_name));
                }

                // sse::on($sse, "close", fn)
                if (fn == "on") {
                    if (argc_s != 3) throw std::runtime_error("sse::on() takes 3 arguments");
                    Value sse_v = evaluate_expression(*e->arguments[0]);
                    std::string ev = evaluate_expression(*e->arguments[1]).to_string();
                    Value cb_v  = evaluate_expression(*e->arguments[2]);
                    if (sse_v.type() != Value::SSE_CONN || !sse_v.as_sse())
                        throw std::runtime_error("sse::on() first argument must be an SSE connection");
                    if (cb_v.type() != Value::FUNCTION)
                        throw std::runtime_error("sse::on() third argument must be a function");
                    auto conn = sse_v.as_sse();
                    if (ev == "close") {
                        auto base = std::shared_ptr<Interpreter>(make_dispatch_copy().release());
                        auto sink = std::make_shared<std::ostringstream>();
                        conn->on_close_cb = [base, sink, cb_v]() mutable {
                            look::WebContext ctx;
                            ctx.method = "__SSE_CLOSE__";
                            base->set_output(*sink);
                            base->set_web_context(&ctx);
                            look::acquire_thread_connections();
                            try { base->invoke(cb_v, {}); } catch (...) {}
                            look::release_thread_connections();
                        };
                    }
                    return Value();
                }

                // sse::close($sse)
                if (fn == "close") {
                    if (argc_s < 1) throw std::runtime_error("sse::close() takes 1 argument");
                    Value sse_v = evaluate_expression(*e->arguments[0]);
                    if (sse_v.type() != Value::SSE_CONN || !sse_v.as_sse())
                        throw std::runtime_error("sse::close() argument must be an SSE connection");
                    sse_v.as_sse()->close_conn();
                    return Value();
                }

                // sse::clients() → count of active SSE connections
                if (fn == "clients") {
                    return Value((int)look::g_sse_registry.count());
                }

                throw std::runtime_error("sse::" + fn + "() not found");
            }

            // ── jobs::run — needs invoke() access, handled here ───────────────
            if (sr->module_name == "jobs" && sr->member_name == "run") {
                int interval_ms = (e->arguments.size() >= 1)
                    ? (int)evaluate_expression(*e->arguments[0]).to_float()
                    : 5000;
                bool once = (interval_ms == 0);

                auto& workers = look::JobStore::instance().workers();
                if (workers.empty())
                    throw std::runtime_error("jobs::run() — önce jobs::worker() ile handler kaydet");

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
                            copy->set_output(*sink);
                            bool ok = false;
                            try {
                                Value result = copy->invoke(fn_v, {job});
                                ok = result.as_bool();
                            } catch (const std::exception& ex) {
                                look::Logger::instance().log(look::LogLevel::LOG_ERROR, "jobs::run",
                                    std::string("handler hatası [") + queue + "]: " + ex.what());
                                ok = false;
                            }

                            if (ok) look::JobStore::instance().done(job_id);
                            else    look::JobStore::instance().fail(job_id);
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

            auto mod_it = modules_.find(sr->module_name);
            if (mod_it == modules_.end())
                throw std::runtime_error("Module '" + sr->module_name + "' not loaded.");
            auto fn_it = mod_it->second.functions.find(sr->member_name);
            if (fn_it == mod_it->second.functions.end())
                throw std::runtime_error("'" + sr->module_name + "' has no function '" + sr->member_name + "'");
            std::vector<Value> args;
            for (const auto& arg : e->arguments) args.push_back(evaluate_expression(*arg));
            return fn_it->second(args);
        }

        auto argc = e->arguments.size();

        // â”€â”€ Built-in array functions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (fn_name == "count" || fn_name == "len") {
            if (argc != 1) throw std::runtime_error(fn_name + "() takes 1 argument");
            Value v = evaluate_expression(*e->arguments[0]);
            if (v.type() == Value::ARRAY) {
                auto& arr = *v.as_array();
                // Assoc arrays store __assoc__ sentinel + key-value pairs
                if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__")
                    return Value((int)((arr.size() - 1) / 2));
                return Value((int)arr.size());
            }
            if (v.type() == Value::STRING) return Value((int)v.to_string().size());
            return Value(0);
        }
        if (fn_name == "push") {
            if (argc != 2) throw std::runtime_error("push() takes 2 arguments");
            Value arr = evaluate_expression(*e->arguments[0]);
            if (arr.type() != Value::ARRAY) throw std::runtime_error("push() requires array as first argument");
            arr.as_array()->push_back(evaluate_expression(*e->arguments[1]));
            return arr;
        }
        if (fn_name == "pop") {
            if (argc != 1) throw std::runtime_error("pop() takes 1 argument");
            Value arr = evaluate_expression(*e->arguments[0]);
            if (arr.type() != Value::ARRAY) throw std::runtime_error("pop() requires array");
            if (arr.as_array()->empty()) return Value();
            Value last = arr.as_array()->back();
            arr.as_array()->pop_back();
            return last;
        }
        if (fn_name == "join") {
            if (argc < 1) throw std::runtime_error("join() takes 1-2 arguments");
            Value arr  = evaluate_expression(*e->arguments[0]);
            std::string sep = (argc >= 2) ? evaluate_expression(*e->arguments[1]).to_string() : "";
            if (arr.type() != Value::ARRAY) throw std::runtime_error("join() requires array");
            std::string result;
            for (size_t i = 0; i < arr.as_array()->size(); ++i) {
                if (i) result += sep;
                result += (*arr.as_array())[i].to_string();
            }
            return Value(result);
        }

        // â”€â”€ env("KEY") / env("KEY", "default") â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // ── Phase 14: Go-style concurrency ───────────────────────────────────────
        // channel([size]) → channel
        if (fn_name == "channel") {
            size_t cap = 128;
            if (argc >= 1) {
                int n = evaluate_expression(*e->arguments[0]).to_int();
                if (n < 0) throw std::runtime_error("channel: kapasite negatif olamaz");
                cap = (n == 0) ? (size_t)-1 : (size_t)n;
            }
            return Value(std::make_shared<LookChannel>(cap));
        }

        // send($ch, $val)
        if (fn_name == "send") {
            if (argc != 2) throw std::runtime_error("send() takes 2 arguments: send($ch, $val)");
            Value ch  = evaluate_expression(*e->arguments[0]);
            Value val = evaluate_expression(*e->arguments[1]);
            if (ch.type() != Value::CHANNEL || !ch.as_channel())
                throw std::runtime_error("send() first argument must be a channel");
            ch.as_channel()->send_val(std::move(val));
            return Value();
        }

        // receive($ch)
        if (fn_name == "receive") {
            if (argc != 1) throw std::runtime_error("receive() takes 1 argument");
            Value ch = evaluate_expression(*e->arguments[0]);
            if (ch.type() != Value::CHANNEL || !ch.as_channel())
                throw std::runtime_error("receive() argument must be a channel");
            return ch.as_channel()->recv_val();
        }

        // close($ch) — kanal kapatma
        if (fn_name == "close") {
            if (argc != 1) throw std::runtime_error("close() takes 1 argument");
            Value ch = evaluate_expression(*e->arguments[0]);
            if (ch.type() != Value::CHANNEL || !ch.as_channel())
                throw std::runtime_error("close() argument must be a channel");
            ch.as_channel()->close_chan();
            return Value();
        }

        // chan_size($ch) → kanal kuyruğundaki eleman sayısı
        if (fn_name == "chan_size") {
            if (argc != 1) throw std::runtime_error("chan_size() takes 1 argument");
            Value ch = evaluate_expression(*e->arguments[0]);
            if (ch.type() != Value::CHANNEL || !ch.as_channel())
                throw std::runtime_error("chan_size() argument must be a channel");
            return Value(ch.as_channel()->sz());
        }

        // parallel(fn) — goroutine: spawn, detach, communicate via channels
        if (fn_name == "parallel") {
            if (argc != 1) throw std::runtime_error("parallel() takes 1 argument");
            Value fn = evaluate_expression(*e->arguments[0]);
            if (fn.type() != Value::FUNCTION)
                throw std::runtime_error("parallel() requires a function");

            goroutine_acquire(); // throws if PARALLEL_MAX_GOROUTINES reached

            auto copy = make_dispatch_copy();
            copy->set_web_context(nullptr);
            auto sink = std::make_shared<std::ostringstream>();
            copy->set_output(*sink);

            std::thread([c = std::move(copy), sink, fn]() mutable {
                GoroutineGuard _guard; // goroutine_release() on scope exit
                try {
                    c->invoke(fn, {});
                } catch (const std::exception& ex) {
                    Logger::instance().log(LogLevel::LOG_ERROR, "parallel",
                        std::string("goroutine panic: ") + ex.what());
                } catch (...) {
                    Logger::instance().log(LogLevel::LOG_ERROR, "parallel",
                        "goroutine panic: unknown error");
                }
            }).detach();

            return Value();
        }

        // jobs::run([interval_ms=5000]) — worker poll loop
        //   interval_ms > 0  → sonsuz döngü (ayrı process / CLI worker)
        //   interval_ms = 0  → tek tur, çık (timer::every ile göm)
        // Needs interpreter access (invoke()) so it lives here, not in jobs_stdlib.cpp.
        if (fn_name == "jobs::run") {
            int interval_ms = (argc >= 1)
                ? (int)evaluate_expression(*e->arguments[0]).to_float()
                : 5000;
            bool once = (interval_ms == 0);

            auto& workers = look::JobStore::instance().workers();
            if (workers.empty())
                throw std::runtime_error("jobs::run() — önce jobs::worker() ile handler kaydet");

            auto run_one_pass = [&]() {
                for (auto& [queue, fn] : workers) {
                    while (true) {
                        Value job = look::JobStore::instance().next(queue);
                        if (job.type() == Value::NONE) break;

                        // Extract id for done()/fail()
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

                        // Invoke handler in dispatch copy (isolated, like parallel())
                        auto copy = make_dispatch_copy();
                        auto sink = std::make_shared<std::ostringstream>();
                        copy->set_output(*sink);
                        bool ok = false;
                        try {
                            Value result = copy->invoke(fn, {job});
                            ok = result.as_bool();
                        } catch (const std::exception& ex) {
                            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "jobs::run",
                                std::string("handler hatası [") + queue + "]: " + ex.what());
                            ok = false;
                        }

                        if (ok) look::JobStore::instance().done(job_id);
                        else    look::JobStore::instance().fail(job_id);
                    }
                }
            };

            if (once) {
                // Single pass — returns immediately (use with timer::every)
                run_one_pass();
                return Value(true);
            }

            // Blocking loop — runs until process is killed (CLI worker mode)
            while (true) {
                run_one_pass();
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
            return Value(); // unreachable
        }

        if (fn_name == "env") {
            ensure_env_loaded();
            if (argc < 1) return Value();
            std::string key = evaluate_expression(*e->arguments[0]).to_string();
            // Check .env first, then system environment
            auto it = g_env_vars.find(key);
            if (it != g_env_vars.end()) return Value(it->second);
            const char* sys = std::getenv(key.c_str());
            if (sys) return Value(std::string(sys));
            // Default value
            if (argc >= 2) return evaluate_expression(*e->arguments[1]);
            return Value();
        }

        // â”€â”€ config("section.key") / config("section.key", "default") â”€â”€â”€â”€â”€
        if (fn_name == "config") {
            ensure_env_loaded();
            if (argc < 1) return Value();
            std::string dotkey = evaluate_expression(*e->arguments[0]).to_string();

            // Parse "section.key"
            auto dot = dotkey.find('.');
            std::string section = (dot != std::string::npos) ? dotkey.substr(0, dot) : dotkey;
            std::string key_    = (dot != std::string::npos) ? dotkey.substr(dot + 1) : "";

            // Config is loaded from .env with prefix: DATABASE_HOST â†’ config("database.host")
            // Transform: "database.host" â†’ "DATABASE_HOST"
            std::string env_key = section + "_" + key_;
            std::transform(env_key.begin(), env_key.end(), env_key.begin(), ::toupper);

            auto it = g_env_vars.find(env_key);
            if (it != g_env_vars.end()) return Value(it->second);
            const char* sys = std::getenv(env_key.c_str());
            if (sys) return Value(std::string(sys));

            if (argc >= 2) return evaluate_expression(*e->arguments[1]);
            return Value();
        }

        // â”€â”€ Web built-ins â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // route("GET", "/path/{id}", function($id) { ... })
        if (fn_name == "route" && web_ctx_) {
            // Pattern parse yardimcisi
            auto parse_pattern = [](const std::string& pat,
                                    std::vector<std::string>& pnames) -> std::string {
                std::string r = "^";
                for (size_t pi = 0; pi < pat.size(); ) {
                    if (pat[pi] == '{') {
                        size_t end = pat.find('}', pi);
                        pnames.push_back(pat.substr(pi + 1, end - pi - 1));
                        r += "([^/]+)";
                        pi = end + 1;
                    } else {
                        char c = pat[pi++];
                        if (std::string(".^$*+?()[]{}|\\").find(c) != std::string::npos)
                            r += "\\";
                        r += c;
                    }
                }
                r += "$";
                return r;
            };

            // route("404", callback)
            if (argc == 2) {
                std::string first = evaluate_expression(*e->arguments[0]).to_string();
                if (first == "404") {
                    Value cb = evaluate_expression(*e->arguments[1]);
                    if (setup_mode_) {
                        RouteEntry entry404;
                        entry404.method   = "404";
                        entry404.is_404   = true;
                        entry404.callback = cb;
                        route_registry_.push_back(std::move(entry404));
                    } else {
                        if (cb.type() == Value::FUNCTION)
                            globals_->define("__404_handler__", cb);
                    }
                    return Value();
                }
            }

            if (argc < 3) throw std::runtime_error("route() requires 3 arguments: method, pattern, callback");
            std::string req_method = evaluate_expression(*e->arguments[0]).to_string();
            std::string pattern    = evaluate_expression(*e->arguments[1]).to_string();
            Value       callback   = evaluate_expression(*e->arguments[2]);

            if (setup_mode_) {
                // Warm start: kaydet, dispatch etme
                RouteEntry entry;
                entry.method   = req_method;
                entry.pattern  = pattern;
                entry.callback = callback;
                std::string rstr = parse_pattern(pattern, entry.param_names);
                entry.pattern_re = std::regex(rstr);
                route_registry_.push_back(std::move(entry));
                return Value();
            }

            // Normal (CGI) modu: aninda dispatch
            if (req_method != web_ctx_->method && req_method != "*") return Value();

            std::vector<std::string> param_names;
            std::string reg = parse_pattern(pattern, param_names);
            std::regex route_re(reg);
            std::smatch match;
            std::string current_path = web_ctx_->path;
            if (!std::regex_match(current_path, match, route_re)) return Value();

            web_ctx_->route_matched = true;
            for (size_t pi = 0; pi < param_names.size(); ++pi)
                web_ctx_->route_params[param_names[pi]] = match[pi + 1].str();

            if (callback.type() == Value::FUNCTION) {
                auto fn = callback.as_function();
                std::vector<Value> args;
                for (size_t pi = 0; pi < param_names.size(); ++pi)
                    args.push_back(Value(match[pi + 1].str()));
                while (args.size() < fn->parameters.size()) args.push_back(Value());
                args.resize(fn->parameters.size());
                call_function(fn, std::move(args));
            }
            throw RouteMatchedException();
        }

        // response(200, "body") â€” set status + optional output
        if (fn_name == "response" && web_ctx_) {
            if (argc >= 1) web_ctx_->set_status(evaluate_expression(*e->arguments[0]).to_int());
            if (argc >= 2) {
                Value body = evaluate_expression(*e->arguments[1]);
                *output_stream_ << body.to_string();
            }
            return Value();
        }

        // redirect("/url")  or  redirect("/url", 301)
        if (fn_name == "redirect" && web_ctx_) {
            if (argc < 1) throw std::runtime_error("redirect() requires URL argument");
            web_ctx_->set_status(argc >= 2 ? evaluate_expression(*e->arguments[1]).to_int() : 302);
            web_ctx_->headers_out["Location"] = evaluate_expression(*e->arguments[0]).to_string();
            return Value();
        }

        // header("Name", "Value")
        if (fn_name == "header" && web_ctx_) {
            if (argc >= 2)
                web_ctx_->headers_out[evaluate_expression(*e->arguments[0]).to_string()]
                    = evaluate_expression(*e->arguments[1]).to_string();
            return Value();
        }

        // json(value) â€” encode to JSON string
        if (fn_name == "json") {
            if (argc < 1) return Value(std::string("null"));
            // Reuse json::encode from web_stdlib
            auto mod_it = modules_.find("json");
            if (mod_it != modules_.end()) {
                std::vector<Value> jargs = { evaluate_expression(*e->arguments[0]) };
                return mod_it->second.functions.at("encode")(jargs);
            }
            return Value(evaluate_expression(*e->arguments[0]).to_string());
        }

        // â”€â”€ Legacy built-ins (still supported) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // exit() / die() — script'i durdurur, FastCGI dongusu devam eder
        if (fn_name == "exit" || fn_name == "die") {
            int code = 0;
            if (argc > 0) {
                Value a = evaluate_expression(*e->arguments[0]);
                if (a.type() == Value::INT) code = a.as_int();
                else if (a.type() == Value::STRING && output_stream_)
                    *output_stream_ << a.as_string(); // die("mesaj") — yaz ve cik
            }
            throw ExitException(code);
        }

        if (fn_name == "str"   || fn_name == "strval" || fn_name == "string") return Value(evaluate_expression(*e->arguments[0]).to_string());
        if (fn_name == "int"   || fn_name == "intval")   return Value(evaluate_expression(*e->arguments[0]).to_int());
        if (fn_name == "float" || fn_name == "floatval") return Value(evaluate_expression(*e->arguments[0]).to_float());
        if (fn_name == "bool"  || fn_name == "boolval")  { Value v = evaluate_expression(*e->arguments[0]); return Value(v.to_int() != 0 || (v.type() == Value::STRING && !v.as_string().empty() && v.as_string() != "false" && v.as_string() != "0")); }
        if (fn_name == "strlen")    { return Value((int)evaluate_expression(*e->arguments[0]).to_string().size()); }
        if (fn_name == "abs")       { Value v = evaluate_expression(*e->arguments[0]); if(v.type()==Value::FLOAT) return Value(std::abs(v.as_float())); return Value(std::abs(v.to_int())); }
        if (fn_name == "max")       { Value a=evaluate_expression(*e->arguments[0]),b=evaluate_expression(*e->arguments[1]); return a>=b?a:b; }
        if (fn_name == "min")       { Value a=evaluate_expression(*e->arguments[0]),b=evaluate_expression(*e->arguments[1]); return a<=b?a:b; }
        if (fn_name == "sqrt")      { return Value(std::sqrt(evaluate_expression(*e->arguments[0]).to_float())); }
        if (fn_name == "strtoupper") { std::string s=evaluate_expression(*e->arguments[0]).to_string(); for(char&c:s)c=toupper((unsigned char)c); return Value(s); }
        if (fn_name == "strtolower") { std::string s=evaluate_expression(*e->arguments[0]).to_string(); for(char&c:s)c=tolower((unsigned char)c); return Value(s); }

        // â”€â”€ User-defined function â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // ── register_builtin() entries (test runner, extensions)
        if (auto bit = builtins_.find(fn_name); bit != builtins_.end()) {
            std::vector<Value> args;
            for (size_t i = 0; i < argc; ++i)
                args.push_back(evaluate_expression(*e->arguments[i]));
            return bit->second(std::move(args));
        }

        Value callee_val = evaluate_expression(*e->callee);
        if (callee_val.type() != Value::FUNCTION && callee_val.type() != Value::BYTECODE_FN)
            throw std::runtime_error("'" + fn_name + "' is not defined");

        if (callee_val.type() == Value::BYTECODE_FN)
            return invoke(callee_val, {});  // VM closure called as bare function

        auto fn = callee_val.as_function();
        std::vector<Value> args;
        for (size_t i = 0; i < argc; ++i)
            args.push_back(evaluate_expression(*e->arguments[i]));
        return call_function(fn, std::move(args));
    }

    throw std::runtime_error("Unknown expression type");
}

Value Interpreter::invoke(const Value& fn, std::vector<Value> args) {
    if (fn.type() != Value::FUNCTION)
        throw std::runtime_error("invoke: not a function");
    return call_function(fn.as_function(), std::move(args));
}

Value Interpreter::call_function(std::shared_ptr<LookFunction> fn, std::vector<Value> args) {
    if (call_depth_ >= MAX_CALL_DEPTH)
        throw LookRuntimeError("Stack overflow: max call depth exceeded in '" + fn->name + "'",
                               current_loc_, call_stack_);

    auto fn_env = std::make_shared<Environment>(fn->closure);

    if (fn->is_variadic) {
        size_t fixed = fn->parameters.size() - 1;
        for (size_t i = 0; i < fixed && i < args.size(); ++i)
            fn_env->define(fn->parameters[i], args[i]);
        auto rest = std::make_shared<std::vector<Value>>();
        for (size_t i = fixed; i < args.size(); ++i)
            rest->push_back(args[i]);
        fn_env->define(fn->parameters[fixed], Value(rest));
    } else {
        if (args.size() != fn->parameters.size())
            throw LookRuntimeError("Function '" + fn->name + "' expects " +
                std::to_string(fn->parameters.size()) + " args, got " + std::to_string(args.size()),
                current_loc_, call_stack_);
        for (size_t i = 0; i < fn->parameters.size(); ++i)
            fn_env->define(fn->parameters[i], args[i]);
    }

    // Push call frame
    call_stack_.push_back({fn->name.empty() ? "<closure>" : fn->name, current_loc_.line});

    auto prev = current_;
    current_  = fn_env;
    ++call_depth_;
    Value result;
    try {
        for (const auto& s : fn->body->statements)
            execute_statement(*s);
    } catch (const ReturnException& ret) {
        result = ret.value();
    } catch (const LookRuntimeError&) {
        // Already enriched — restore env and rethrow, keep frame in error
        --call_depth_;
        current_ = prev;
        call_stack_.pop_back();
        throw;
    } catch (const std::runtime_error& e) {
        // Enrich with current location + full call stack (captured before pop)
        auto err = LookRuntimeError(e.what(), current_loc_, call_stack_);
        --call_depth_;
        current_ = prev;
        call_stack_.pop_back();
        throw err;
    } catch (...) {
        --call_depth_;
        current_ = prev;
        call_stack_.pop_back();
        throw;
    }
    --call_depth_;
    current_ = prev;
    call_stack_.pop_back();
    return result;
}


void Interpreter::dispatch_routes() {
    if (!web_ctx_) return;
    ++request_count_;

    // Eslesen route'u bul ve cagir
    for (auto& entry : route_registry_) {
        if (entry.is_404) continue;
        if (entry.method != web_ctx_->method && entry.method != "*") continue;

        std::smatch match;
        if (!std::regex_match(web_ctx_->path, match, entry.pattern_re)) continue;

        // Route eslestti
        web_ctx_->route_matched = true;
        web_ctx_->route_params.clear();
        for (size_t pi = 0; pi < entry.param_names.size(); ++pi)
            web_ctx_->route_params[entry.param_names[pi]] = match[pi + 1].str();

        if (entry.callback.type() == Value::FUNCTION) {
            auto fn = entry.callback.as_function();
            std::vector<Value> args;
            // Phase 15: WS routes receive $ws as first argument
            if (entry.method == "WS" && ws_conn_)
                args.push_back(Value(ws_conn_));
            // Phase 16: SSE routes receive $sse as first argument
            if (entry.method == "SSE" && sse_conn_)
                args.push_back(Value(sse_conn_));
            for (size_t pi = 0; pi < entry.param_names.size(); ++pi)
                args.push_back(Value(match[pi + 1].str()));
            while (args.size() < fn->parameters.size()) args.push_back(Value());
            args.resize(fn->parameters.size());
            call_function(fn, std::move(args));
        }
        return;
    }

    // Eslesen route yok — 404 handler
    web_ctx_->set_status(404);
    for (auto& entry : route_registry_) {
        if (!entry.is_404) continue;
        if (entry.callback.type() == Value::FUNCTION)
            invoke(entry.callback, {});
        return;
    }
    // 404 handler tanimli degil — varsayilan mesaj
    *output_stream_ << "{\"ok\":false,\"hata\":\"Endpoint bulunamadi\"}";
}
// ── look_get_env — VM setup için .env-aware env() erişimi ───────────────────
std::string look_get_env(const std::string& key, const std::string& default_val) {
    ensure_env_loaded();
    auto it = g_env_vars.find(key);
    if (it != g_env_vars.end()) return it->second;
    const char* sys = std::getenv(key.c_str());
    if (sys) return std::string(sys);
    return default_val;
}

} // namespace look


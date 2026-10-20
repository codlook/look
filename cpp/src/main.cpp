#include "look/lexer.h"
#include "look/parser.h"
#include "look/capture_check.h"
#include "look/arg_check.h"
#include "look/format_src.h"   // lk fmt
#include "look/interpreter.h"
#include "look/array_count.h"
#include "look/http_client.h"
#include "look/runtime_init.h"    // runtime_init (CA probe + SQLite init, süreç başı)
#include "look/test_runner.h"
#include "look/repl.h"
#include "look/web.h"
#include "look/installer.h"
#include "look/parallel_runtime.h"
#include "look/ast.h"
#include "look/builtins.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/use_modules.h"   // collect_use_modules — CLI ve web aynı `use` listesi
#include "look/timer.h"         // TimerManager — komut satırında timer:: VM closure'ı kurar
#include "look/setup_handoff.h" // vm_closure_runner — jobs::run işleyicilerini VM'de koşturur
#include <memory>
#include <unordered_map>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normalde CMake compile-def sağlar (tek kaynak)
#endif
#ifndef LOOK_BUILD          // CMake damgası yoksa (ham derleme) güvenli varsayılan
#define LOOK_BUILD "src"
#endif

#ifdef _WIN32
#  define LOOK_PLATFORM "windows"
#elif defined(__linux__)
#  define LOOK_PLATFORM "linux"
#elif defined(__APPLE__)
#  define LOOK_PLATFORM "darwin"
#else
#  define LOOK_PLATFORM "unknown"
#endif

#ifdef __x86_64__
#  define LOOK_ARCH "amd64"
#elif defined(__aarch64__)
#  define LOOK_ARCH "arm64"
#elif defined(_M_X64)
#  define LOOK_ARCH "amd64"
#else
#  define LOOK_ARCH "x86"
#endif

static void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  lk <source.lk>                  — run a script\n";
    std::cout << "  lk -c \"code\"                    — run inline code\n";
    std::cout << "  lk --check <source.lk>           — parse only (no run); report errors\n";
    std::cout << "  lk test                          — run all tests in tests/\n";
    std::cout << "  lk test <pattern>                — run matching tests\n";
    std::cout << "  lk test --verbose                — verbose output\n";
    std::cout << "  lk repl                          — interactive REPL\n";
    std::cout << "  lk module install <github.com/user/repo>  — install module from GitHub\n";
    std::cout << "  lk module list                            — list official modules\n";
    std::cout << "  lk install <pkg>                 — install package (e.g. github.com/codlook/look-packages/firebase)\n";
    std::cout << "  lk install <pkg@ref>             — install specific branch/tag\n";
    std::cout << "  lk install                       — install all from look.lock\n";
    std::cout << "  lk install [pkg] --locked        — install the exact commit in look.lock (reproducible)\n";
    std::cout << "  lk version                       — print version info\n";
}

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Unable to open file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// ── Semantik kontrol (yalniz --check) — tanimsiz bare fonksiyon cagrilari ──────
// Guvenli/yanlis-pozitifsiz olmak icin: yalniz CallExpression'in callee'si bare
// ($'siz, ::'siz) Variable olanlari; gecerli kume = interpreter'in inline bare
// fonksiyonlari + builtin_names() + dosyadaki fn/struct/const adlari. Dosyada
// `use` importu varsa (cross-file isimler bilinemez) hic kontrol edilmez.
namespace {
using namespace look;

void walk_expr(const Expression* e, const std::function<void(const Expression*)>& v);
void walk_block(const BlockStatement* b, const std::function<void(const Expression*)>& v);

void walk_stmt(const Statement* s, const std::function<void(const Expression*)>& v) {
    if (!s) return;
    if (auto* x = dynamic_cast<const ExpressionStatement*>(s)) walk_expr(x->expression.get(), v);
    else if (auto* x = dynamic_cast<const PrintStatement*>(s)) { for (auto& e : x->expressions) walk_expr(e.get(), v); }
    else if (auto* x = dynamic_cast<const WriteStatement*>(s)) { for (auto& e : x->expressions) walk_expr(e.get(), v); }
    else if (auto* x = dynamic_cast<const ReturnStatement*>(s)) walk_expr(x->expression.get(), v);
    else if (auto* x = dynamic_cast<const BlockStatement*>(s)) walk_block(x, v);
    else if (auto* x = dynamic_cast<const IfStatement*>(s)) { walk_expr(x->condition.get(), v); walk_block(x->then_branch.get(), v); walk_block(x->else_branch.get(), v); }
    else if (auto* x = dynamic_cast<const WhileStatement*>(s)) { walk_expr(x->condition.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = dynamic_cast<const ForStatement*>(s)) { walk_stmt(x->init.get(), v); walk_expr(x->condition.get(), v); walk_expr(x->post.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = dynamic_cast<const ForeachStatement*>(s)) { walk_expr(x->iterable.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = dynamic_cast<const TryCatchStatement*>(s)) { walk_block(x->try_block.get(), v); walk_block(x->catch_block.get(), v); walk_block(x->finally_block.get(), v); }
    else if (auto* x = dynamic_cast<const SwitchStatement*>(s)) { walk_expr(x->subject.get(), v); for (auto& c : x->cases) { for (auto& val : c.values) walk_expr(val.get(), v); for (auto& st : c.body) walk_stmt(st.get(), v); } }
    else if (auto* x = dynamic_cast<const FunctionDeclaration*>(s)) walk_block(x->body.get(), v);
}

void walk_block(const BlockStatement* b, const std::function<void(const Expression*)>& v) {
    if (!b) return;
    for (auto& s : b->statements) walk_stmt(s.get(), v);
}

void walk_expr(const Expression* e, const std::function<void(const Expression*)>& v) {
    if (!e) return;
    v(e);
    if (auto* x = dynamic_cast<const ArrayLiteral*>(e)) { for (auto& el : x->elements) walk_expr(el.get(), v); }
    else if (auto* x = dynamic_cast<const AssocArrayLiteral*>(e)) { for (auto& p : x->pairs) { walk_expr(p.first.get(), v); walk_expr(p.second.get(), v); } }
    else if (auto* x = dynamic_cast<const FunctionExpression*>(e)) walk_block(x->body.get(), v);
    else if (auto* x = dynamic_cast<const IndexExpression*>(e)) { walk_expr(x->object.get(), v); walk_expr(x->index.get(), v); }
    else if (auto* x = dynamic_cast<const UnaryExpression*>(e)) walk_expr(x->right.get(), v);
    else if (auto* x = dynamic_cast<const BinaryExpression*>(e)) { walk_expr(x->left.get(), v); walk_expr(x->right.get(), v); }
    else if (auto* x = dynamic_cast<const AssignmentExpression*>(e)) { walk_expr(x->index.get(), v); walk_expr(x->value.get(), v); }
    else if (auto* x = dynamic_cast<const CallExpression*>(e)) { walk_expr(x->callee.get(), v); for (auto& a : x->arguments) walk_expr(a.get(), v); }
    else if (auto* x = dynamic_cast<const TernaryExpression*>(e)) { walk_expr(x->condition.get(), v); walk_expr(x->then_expr.get(), v); walk_expr(x->else_expr.get(), v); }
    else if (auto* x = dynamic_cast<const MemberAccessExpression*>(e)) walk_expr(x->object.get(), v);
    else if (auto* x = dynamic_cast<const StructLiteralExpression*>(e)) { for (auto& f : x->fields) walk_expr(f.second.get(), v); }
}

void collect_decls(const Statement* s, std::set<std::string>& d) {
    if (!s) return;
    if (auto* f = dynamic_cast<const FunctionDeclaration*>(s)) { d.insert(f->name); if (f->body) for (auto& st : f->body->statements) collect_decls(st.get(), d); }
    else if (auto* st = dynamic_cast<const StructDeclaration*>(s)) d.insert(st->name);
    else if (auto* cb = dynamic_cast<const ConstBlock*>(s)) { for (auto& it : cb->items) d.insert(it.name); }
    else if (auto* b = dynamic_cast<const BlockStatement*>(s)) { for (auto& x : b->statements) collect_decls(x.get(), d); }
    else if (auto* i = dynamic_cast<const IfStatement*>(s)) { if (i->then_branch) for (auto& x : i->then_branch->statements) collect_decls(x.get(), d); if (i->else_branch) for (auto& x : i->else_branch->statements) collect_decls(x.get(), d); }
    else if (auto* w = dynamic_cast<const WhileStatement*>(s)) { if (w->body) for (auto& x : w->body->statements) collect_decls(x.get(), d); }
    else if (auto* fr = dynamic_cast<const ForStatement*>(s)) { if (fr->body) for (auto& x : fr->body->statements) collect_decls(x.get(), d); }
    else if (auto* fe = dynamic_cast<const ForeachStatement*>(s)) { if (fe->body) for (auto& x : fe->body->statements) collect_decls(x.get(), d); }
    else if (auto* t = dynamic_cast<const TryCatchStatement*>(s)) { if (t->try_block) for (auto& x : t->try_block->statements) collect_decls(x.get(), d); if (t->catch_block) for (auto& x : t->catch_block->statements) collect_decls(x.get(), d); if (t->finally_block) for (auto& x : t->finally_block->statements) collect_decls(x.get(), d); }
    else if (auto* sw = dynamic_cast<const SwitchStatement*>(s)) { for (auto& c : sw->cases) for (auto& st : c.body) collect_decls(st.get(), d); }
}

bool find_undefined_call(const Program& prog, std::string& name, int& line, int& col) {
    for (auto& s : prog.statements)
        if (dynamic_cast<const UseStatement*>(s.get()) || dynamic_cast<const UseFileStatement*>(s.get()))
            return false;   // import var → cross-file isimler bilinemez, kontrol etme

    // Gecerli bare cagri isimleri: interpreter'in inline fonksiyonlari +
    // builtin_names() bare girdileri + comert ekstralar (yanlis pozitif olmasin).
    std::set<std::string> valid = {
        "abs","args","before_route","bool","boolval","chan_size","channel","close","config","count",
        "die","env","exit","float","floatval","header","int","intval","join","json","len","max",
        "min","parallel","pop","push","receive","redirect","response","route","send","sqrt","stop",
        "str","string","strlen","strtolower","strtoupper","strval",
        "print","write","is_array","is_bool","is_float","is_int","is_null","is_string",
        "json_encode","json_decode",
        "keys","values","type","range","isset","unset","sort","reverse","map","filter","reduce",
        "slice","contains","jwt_sign","jwt_verify","jwt_decode"
    };
    for (auto& s : prog.statements) collect_decls(s.get(), valid);

    bool found = false;
    std::function<void(const Expression*)> visit = [&](const Expression* e) {
        if (found) return;
        auto* call = dynamic_cast<const CallExpression*>(e);
        if (!call) return;
        auto* var = dynamic_cast<const Variable*>(call->callee.get());
        if (!var) return;
        const std::string& n = var->name;
        if (n.empty() || n[0] == '$') return;
        if (n.find("::") != std::string::npos) return;
        if (valid.count(n)) return;
        found = true; name = n; line = var->loc.line; col = var->loc.column;
    };
    for (auto& s : prog.statements) walk_stmt(s.get(), visit);
    return found;
}
// ── C9 (CLI-VM, opt-in) — CLI top-level script'i bytecode VM'de çalıştırmak için
// builtin tablosu. http_main req_builtins ile aynı semantik; çıktı `out`'a gider.
// Modül fn'leri interpreter'ın YÜKLÜ modüllerinden auto-wire olur (use gerektirir).
static std::vector<look::BuiltinFn> build_cli_builtins(look::Interpreter& interp, std::ostream& out) {
    using look::Value;
    std::vector<look::BuiltinFn> b(look::builtin_names().size());
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    b[0] = [&out](std::vector<Value>& a) -> Value { for (auto& x : a) out << x.to_string(); return Value(); };
    b[1] = b[0];  // print/write
    b[2] = [](std::vector<Value>& a) -> Value {   // count/len — tek tanım (array_count.h)
        return Value(a.empty() ? 0 : look_count(a[0]));   // assoc: (size-1)/2 (sentinel atlanır)
    };
    b[5] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? std::string() : a[0].to_string()); };  // str
    b[6] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); return Value(a[0].to_int()); };  // int — to_int(): stoll(to_string(float)) bilimsel-gösterim bug'ı + int32 daralması kapandı (tree-walk parite)
    b[7] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0.0); return Value(a[0].to_float()); };  // float — to_float(): to_string() round-trip yok (tree-walk parite)
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
    b[22] = [](std::vector<Value>&) -> Value { return Value(); };  // route — CLI'da dispatch yok
    b[BI("strlen")] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? 0 : (int)a[0].to_string().size()); };
    b[BI("abs")] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); if (a[0].type()==Value::FLOAT) return Value(std::abs(a[0].as_float())); return Value(std::abs(a[0].to_int())); };
    b[BI("max")] = [](std::vector<Value>& a) -> Value { if (a.size()<2) return a.empty()?Value():a[0]; return a[0]>=a[1]?a[0]:a[1]; };
    b[BI("min")] = [](std::vector<Value>& a) -> Value { if (a.size()<2) return a.empty()?Value():a[0]; return a[0]<=a[1]?a[0]:a[1]; };
    b[BI("sqrt")] = [](std::vector<Value>& a) -> Value { return Value(a.empty()?0.0:std::sqrt(a[0].to_float())); };
    b[BI("strtoupper")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::toupper((unsigned char)c); return Value(s); };
    b[BI("strtolower")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::tolower((unsigned char)c); return Value(s); };
    b[BI("push")] = [](std::vector<Value>& a) -> Value { if (a.size()<2||a[0].type()!=Value::ARRAY) throw std::runtime_error("push() requires array and value"); a[0].as_array()->push_back(a[1]); return a[0]; };
    b[BI("pop")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) throw std::runtime_error("pop() requires array"); auto ar=a[0].as_array(); if (ar->empty()) return Value(); Value l=ar->back(); ar->pop_back(); return l; };
    b[BI("join")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) return Value(a.empty()?std::string():a[0].to_string()); std::string sep=a.size()>=2?a[1].to_string():""; std::string r; auto& ar=*a[0].as_array(); for(size_t i=0;i<ar.size();++i){ if(i)r+=sep; r+=ar[i].to_string(); } return Value(r); };
    b[BI("stop")] = [](std::vector<Value>&) -> Value { return Value(); };
    // exit()/die() — CLI-VM'de bağlı değildi ("Not callable" fırlatıyordu).
    // interpreter ile birebir: ilk argüman INT ise çıkış kodu, yoksa 0.
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
            auto arr = std::make_shared<std::vector<Value>>();
            for (const auto& s : look::script_args()) arr->push_back(Value(s));
            return Value(arr);
        };
    // channel() — CLI-VM'de BAĞLI DEĞİLDİ: send/receive'in CHAN_* opcode'ları var ama
    // kanal OLUŞTURMA düz builtin çağrısı → VM tablosunda boş → "kullanilamiyor
    // (baglanmamis)". tree-walk (interpreter.cpp:1631) inline hallettiği için CLI'da
    // parallel+channel tree-walk'ta çalışıp VM'de (DEFAULT) çöküyordu. Semantik birebir:
    // default 128, n<0 hata, n==0 → unbuffered ((size_t)-1).
    if (look::builtin_index("channel") >= 0)
        b[BI("channel")] = [](std::vector<Value>& a) -> Value {
            size_t cap = 128;
            if (!a.empty()) {
                int n = a[0].to_int();
                if (n < 0) throw std::runtime_error("channel: capacity cannot be negative");
                cap = (n == 0) ? (size_t)-1 : (size_t)n;
            }
            return Value(std::make_shared<look::LookChannel>(cap));
        };
    // config() — channel ile AYNI SINIF: web'de (http_main) bağlı, CLI-VM'de eksikti →
    // tree-walk "def" dönerken VM "kullanilamiyor" veriyordu. tree-walk (interpreter.cpp:1802)
    // "section.key" → "SECTION_KEY" env anahtarına çevirip g_env_vars+getenv'e bakar; look_get_env
    // AYNI kaynağı kullanır → birebir parite. Bulunamaz+default-yok → null (sentinel ile ayırt).
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
    // Modül fn'leri: builtin_names'deki her "mod::fn" → interpreter'ın YÜKLÜ modülü.
    // YALNIZCA `use` edilmiş modüller auto-wire olur — interpreter ile parity: interpreter
    // de string::/math::/array:: için `use` gerektirir (json:: gibi her-zaman-açık olanlar
    // ayrı, index-tabanlı wire edilir). Preload YOK — aksi halde VM `use`'suz çalışıp
    // interpreter hata verirdi (divergence).
    const auto& names = look::builtin_names();
    for (size_t i = 0; i < names.size(); ++i) {
        auto pos = names[i].find("::");
        if (pos == std::string::npos) continue;
        auto f = interp.get_module_fn(names[i].substr(0, pos), names[i].substr(pos + 2));
        if (f) b[i] = [f](std::vector<Value>& args) -> Value { std::vector<Value> aa = args; return f(aa); };
    }
    return b;
}

// ── LOOK 2: komut satırında geri çağırmalar da VM'de koşar ────────────────────────────────────
// timer::after/every ve jobs::worker işleyicisi eskiden betiği bütünüyle tree-walk'a düşürüyordu.
// Şimdi her çağrı kendi VM'inde koşar (web'deki gibi: istek başına yeni VM), aynı yerleşik tabloyla.
//   * iş işleyicisi jobs::run içinde, AYNI iş parçacığında koşar → ana VM'in GÜNCEL global'lerini görür
//   * zamanlayıcı başka iş parçacığında koşar → kurulduğu andaki global'lerin kopyasını görür
// Çıktı stdout'a gider. ESKİ HATA: yorumlayıcı işleyicinin/zamanlayıcının çıktısını ayrı bir tampona
// yazıp atıyordu — komut satırındaki bir iş betiğinde `print` sessizce kayboluyordu.
struct CliCallbacks {
    look::VM::SharedState* shared  = nullptr;   // yerleşik tablo (süreç ömrü)
    look::VM*              main_vm = nullptr;   // jobs::run anında güncel global'ler
};
static look::Value run_cli_closure(const CliCallbacks& cc, const look::Value& fnv, std::vector<look::Value> args,
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
static void wire_cli_callbacks(std::vector<look::BuiltinFn>& b, look::Interpreter& interp,
                               std::shared_ptr<CliCallbacks> cc) {
    using look::Value;
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    // jobs::run işleyicileri vm_closure_runner üzerinden çağırır (BYTECODE_FN).
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
            // Zamanlayıcı başka iş parçacığında koşar: global'lerin o anki kopyası (paylaşım yok).
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
    // ws::/sse:: yalnız bir web uygulamasının bağlantı rotasında anlamlı — açık hata.
    for (const auto& n : look::builtin_names()) {
        if (n.rfind("ws::", 0) != 0 && n.rfind("sse::", 0) != 0) continue;
        b[BI(n.c_str())] = [n](std::vector<Value>&) -> Value {
            throw std::runtime_error(n + "() can be used only in a web application (lk-fcgi), not on the command line");
        };
    }
}

} // namespace

int main(int argc, char* argv[]) {
    look::runtime_init();  // tüm süreç-başı init (CA probe + SQLite bir-kez init) — 3 giriş noktası ortak
    try {
        if (argc < 2) { print_usage(); return 1; }

        std::string cmd = argv[1];

        // ── lk --check <file> : yalnizca parse et (CALISTIRMA), hatalari
        //    makine-okunur bicimde bildir. Editor diagnostics icin — yan etki yok.
        bool check_only = false;
        if (cmd == "--check") {
            check_only = true;
            if (argc < 3) { std::cerr << "usage: lk --check <file>\n"; return 2; }
            cmd = argv[2];
        }

        // lk --builtins : the VM's built-in table, one name per line (tests generate their cases from it).
        if (cmd == "--builtins") {
            for (const auto& n : look::builtin_names()) std::cout << n << "\n";
            return 0;
        }

        // lk --vm-report : which module functions the VM does not provide. LOOK 2 runs only on the
        // VM (web and command line), so a program that reaches one of them does not start; the
        // list has to be empty (CI).
        if (cmd == "--vm-report") {
            look::WebContext rctx;
            look::Interpreter rint;
            rint.set_web_context(&rctx);
            std::set<std::string> known(look::builtin_names().begin(), look::builtin_names().end());
            size_t total = 0, missing = 0;
            std::map<std::string, std::vector<std::string>> by_module;
            for (const auto& [mname, mod] : rint.stdlib_modules())
                for (const auto& [fname, fn] : mod.functions) {
                    ++total;
                    if (!known.count(mname + "::" + fname)) { ++missing; by_module[mname].push_back(fname); }
                }
            for (const auto& [mname, fns] : by_module) {
                std::cout << mname << " (" << fns.size() << "):";
                for (const auto& f : fns) std::cout << " " << f;
                std::cout << "\n";
            }
            std::cout << "module functions: " << total << ", not provided by the VM: " << missing << "\n";
            return missing == 0 ? 0 : 1;
        }

        if (cmd == "version" || cmd == "--version" || cmd == "-v") {
            std::cout << "LOOK " << LOOK_VERSION
                      << " (" << LOOK_BUILD << ", " << LOOK_PLATFORM << "/" << LOOK_ARCH << ")"
                      << "\n";
            return 0;
        }

        if (cmd == "repl") {
            return look::run_repl();
        }

        if (cmd == "test") {
            std::string pattern;
            bool verbose = false;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--verbose" || arg == "-v") verbose = true;
                else if (pattern.empty()) pattern = arg;
            }
            return look::run_test_mode(pattern, verbose);
        }

        // ── lk fmt <file...> [--check] : kaynağı kanonik biçime getir (gofmt modeli) ──
        // --check → CI modu: biçimsiz dosya varsa exit 1, yazMAZ. Dosyasız → stdin/stdout.
        if (cmd == "fmt") {
            bool fmt_check = false;
            std::vector<std::string> files;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--check") fmt_check = true;
                else files.push_back(arg);
            }
            return look::run_fmt(files, fmt_check);
        }

        // ── lk module <sub> ──────────────────────────────────────────────────
        if (cmd == "module") {
            if (argc < 3) {
                std::cout << "Usage:\n"
                          << "  lk module install <github.com/user/repo>  — install a module\n"
                          << "  lk module list                             — list official modules\n";
                return 1;
            }
            std::string sub = argv[2];
            bool verbose = false;
            std::string pkg_url;
            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--verbose" || arg == "-v") verbose = true;
                else if (pkg_url.empty()) pkg_url = arg;
            }
            if (sub == "install") {
                if (pkg_url.empty()) {
                    std::cerr << "Error: a GitHub link is required.\n"
                              << "Example: lk module install github.com/codlook/look-modules/jwt\n";
                    return 1;
                }
                return look::cmd_module_install(pkg_url, verbose);
            }
            if (sub == "list") {
                return look::cmd_module_list();
            }
            std::cerr << "Unknown subcommand: " << sub << "\n";
            return 1;
        }

        // ── lk install [pkg] ─────────────────────────────────────────────────
        if (cmd == "install") {
            bool verbose = false;
            bool locked  = false;
            std::string pkg;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--verbose" || arg == "-v") verbose = true;
                else if (arg == "--locked" || arg == "--frozen") locked = true;
                else if (pkg.empty()) pkg = arg;
            }
            if (pkg.empty()) return look::cmd_install_all(verbose, locked);
            return look::cmd_install(pkg, verbose, locked);
        }

        // ── lk <file> / lk -c "code" ─────────────────────────────────────────
        std::string filename = (cmd != "-c") ? cmd : "<inline>";
        std::string source;
        if (cmd == "-c") {
            if (argc < 3) { print_usage(); return 1; }
            source = argv[2];
            for (int i = 3; i < argc; i++) look::script_args().push_back(argv[i]);
        } else {
            source = read_file(cmd);
            // `lk file.lk <args...>` — script'ten sonraki kelimeler args()'a gider.
            for (int i = 2; i < argc; i++) look::script_args().push_back(argv[i]);
        }

        look::Lexer  lexer(source);
        auto         tokens  = lexer.scan_tokens();
        look::Parser parser(std::move(tokens));
        std::unique_ptr<look::Program> program;
        try {
            program = parser.parse();
        } catch (const look::LookParseError& e) {
            if (check_only) {
                std::cout << "CHECK " << (e.line > 0 ? e.line : 1) << " "
                          << (e.column > 0 ? e.column : 1) << " " << e.message << "\n";
                return 1;
            }
            auto err = e;
            if (err.file.empty()) err.file = filename;
            std::cerr << err.format();
            return 1;
        } catch (const std::runtime_error& e) {
            if (check_only) { std::cout << "CHECK 1 1 " << e.what() << "\n"; return 1; }
            std::cerr << "\nParse Error: " << e.what()
                      << "\n  File: " << filename << "\n";
            return 1;
        }

        if (!check_only) look::log_param_writes(parser.warnings(), filename);

        // Parse basarili — --check modunda: tanimsiz cagri semantik kontrolu,
        // sonra calistirmadan cik (yan etki yok).
        if (check_only) {
            std::string bad; int bl = 0, bc = 0;
            if (find_undefined_call(*program, bad, bl, bc)) {
                std::cout << "CHECK " << (bl > 0 ? bl : 1) << " " << (bc > 0 ? bc : 1)
                          << " Undefined function: " << bad << "\n";
                return 1;
            }
            // LOOK 2 geçiş denetimi: closure yakalama kuralının kıracağı yerler (şimdilik uyarı;
            // çıkış kodu değişmez, editör eklentisi yalnız "CHECK" satırına bakar).
            for (const auto& w : look::check_captures(*program))
                std::cout << "WARN " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                          << " [" << w.kind << "] " << w.message << "\n";
            // LOOK 2 geçiş denetimi: adlı fonksiyona yanlış sayıda argümanla çağrı (iki motor
            // bugün ayrışıyor; kural kesinleşmeden önce sayım için).
            for (const auto& w : look::check_arg_counts(*program))
                std::cout << "WARN " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                          << " [arg-count] " << w.message << "\n";
            // LOOK 2: parametreye yazıp döndürmeyen fonksiyon — burada HATA (CI için); yüklemede uyarı.
            {
                bool bad_param = false;
                for (const auto& w : parser.warnings()) {
                    if (w.kind != "param-write") continue;
                    std::cout << "CHECK " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                              << " [param-write] " << w.message << "\n";
                    bad_param = true;
                }
                if (bad_param) return 1;
            }
            // VM bu dosyayı derleyebiliyor mu? LOOK 2'de derlenemeyen program ÇALIŞMAZ (komut satırı ve
            // web) — burada adıyla söylenir, hata. Çalıştırma yok.
            // Temel dizin web yolundaki gibi verilir: `use "dosya.lk"` dahil edilenler de derlenir.
            try {
                auto checked = look::Compiler::compile(*program, std::filesystem::absolute(filename).parent_path().string());
                // Var olmayan modül fonksiyonu: `string::uper(...)`. Yalnız ÇEKİRDEK modüller için söylenir
                // (adı yerleşik tablosunda geçen modüller) — paket modüllerinin fonksiyonları burada bilinmez.
                std::set<std::string> core_modules;
                for (const auto& n : look::builtin_names()) { auto p = n.find("::"); if (p != std::string::npos) core_modules.insert(n.substr(0, p)); }
                bool unknown_fn = false;
                for (const auto& n : checked.non_builtin_module_fns) {
                    auto p = n.find("::");
                    if (p == std::string::npos || !core_modules.count(n.substr(0, p))) continue;
                    std::cout << "CHECK 1 1 [undefined-function] " << n << "() does not exist — check the spelling of the module and the function\n";
                    unknown_fn = true;
                }
                if (unknown_fn) return 1;
            }
            catch (const std::exception& ex) {
                // Tek başına denetlenen bir alt dosyada `use "../x.lk"` uygulama kökü bilinmeden
                // çözülemez (kök, giriş dosyasının dizinidir) — bu VM eksiği değil, raporlanmaz.
                const std::string why = ex.what();
                if (why.find("the file cannot be opened") == std::string::npos &&
                    why.find("outside the application directory") == std::string::npos) {
                    auto* ce = dynamic_cast<const look::LookCompileError*>(&ex);
                    const int line = ce && ce->line > 0 ? ce->line : 1;
                    std::cout << "CHECK " << line << " 1 [compile] " << why << "\n";
                    return 1;
                }
            }
            std::cout << "OK\n";
            return 0;
        }

        look::WebContext web_ctx;
        web_ctx.method = "GET";
        web_ctx.path   = "/";

        look::Interpreter interpreter;
        interpreter.set_web_context(&web_ctx);
        interpreter.set_file(filename);

        try {
            // LOOK 2 tek motor: komut satırı YALNIZ VM'de çalışır. Eskiden derleme hatası, paket
            // modülü, timer::/jobs::worker ya da VM'de olmayan bir mod::fn betiği sessizce (sonra
            // bir uyarıyla) ~40× yavaş tree-walk'a düşürüyordu; LOOK_CLI_VM=0 onu seçtiriyordu.
            // Artık: derlenemeyen program ÇALIŞMAZ (nedeni yazılır, çıkış 1); LOOK_CLI_VM=0 ve
            // LOOK_BYTECODE=0 bir uyarıyla yok sayılır (web sunucusuyla aynı).
            for (const char* var : {"LOOK_CLI_VM", "LOOK_BYTECODE"}) {
                const char* v = std::getenv(var);
                if (v && v[0] == '0')
                    look::Logger::instance().log(look::LogLevel::LOG_WARN, "VM",
                        std::string(var) + "=0 is ignored: LOOK 2 runs only on the VM.");
            }
            const std::filesystem::path script_dir = std::filesystem::absolute(filename).parent_path();
            // Yalnız `use` edilen stdlib modülleri açılır (web ile aynı liste). Paket modülü
            // (~/.look/modules) stdlib değildir: derleyici derler; bilinmeyen modülü derleyici reddeder.
            {
                std::set<std::string> seen_files;
                std::vector<std::string> mods;
                look::collect_use_modules(*program, script_dir, seen_files, mods);
                for (const auto& m : mods) interpreter.load_stdlib_module(m);
            }
            look::CompiledProgram compiled;
            try {
                compiled = look::Compiler::compile(*program, script_dir.string());
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n  File: " << filename << std::endl;
                return 1;
            }
            // VM'de karşılığı olmayan bir mod::fn adı yazım hatasıdır (web'deki "does not exist"
            // kuralıyla aynı) — çalıştırmadan söylenir.
            if (compiled.uses_non_builtin_module_fn) {
                for (const auto& n : compiled.non_builtin_module_fns)
                    std::cerr << "Error: " << n << "() does not exist — check the spelling of the module and the function.\n";
                std::cerr << "  File: " << filename << std::endl;
                return 1;
            }
            // Yerleşik tablo ve paylaşılan durum SÜREÇ ÖMRÜ: zamanlayıcı betik bittikten sonra da
            // (parallel görevleri beklenirken) tetiklenebilir.
            static std::vector<look::BuiltinFn> cli_builtins;
            static look::VM::SharedState sh;
            cli_builtins = build_cli_builtins(interpreter, std::cout);
            sh.builtins = &cli_builtins;
            auto callbacks = std::make_shared<CliCallbacks>();
            callbacks->shared = &sh;
            wire_cli_callbacks(cli_builtins, interpreter, callbacks);
            look::VM vm(sh, std::cout);
            vm.set_web_context(&web_ctx);
            callbacks->main_vm = &vm;
            struct MainVmGone { std::shared_ptr<CliCallbacks> c; ~MainVmGone() { c->main_vm = nullptr; } } main_vm_gone{callbacks};
            // Yakalanmamış hata tree-walk'un biçiminde raporlanır: VM'in kaydettiği satır
            // (proto.lines) LookRuntimeError'a sarılır. ExitException/RouteMatched kontrol akışıdır.
            try {
                vm.execute(compiled);
            } catch (const look::ExitException&)         { throw; }
              catch (const look::RouteMatchedException&) { throw; }
              catch (const look::LookRuntimeError&)      { throw; }  // zaten konumlu
              catch (const std::exception& ex) {
                look::SourceLocation loc;
                loc.file = filename;
                loc.line = vm.last_error_line();
                throw look::LookRuntimeError(ex.what(), loc, {});
            }
        } catch (const look::RouteMatchedException&) {
            // route() flow control — normal
        } catch (const look::ExitException& e) {
            // exit()/die() — NORMAL sonlanma, hata değil. Yakalanmıyordu: dıştaki genel
            // catch'e düşüp "Error: exit" basıyor ve kodu 1 döndürüyordu (exit(3) → 1).
            // cgi_main/fcgi_main zaten yakalıyordu — CLI eksikti.
            look::task_wait(5000);   // arka plan parallel() task'larını bekle
            return e.code();
        } catch (const look::LookRuntimeError& e) {
            auto err = e;
            if (err.location.file.empty()) err.location.file = filename;
            std::cerr << err.format() << std::endl;
            look::task_wait(3000); // drain parallel tasks before exit on error
            return 1;
        }

        // Wait for any background parallel() tasks to finish.
        // 5 s timeout — detached tasks that overstay are abandoned.
        look::task_wait(5000);
        return 0;
    } catch (const look::LookRuntimeError& e) {
        std::cerr << e.format() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

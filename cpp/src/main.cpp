#include "look/lexer.h"
#include "look/parser.h"
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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

#define LOOK_VERSION "1.0.0"
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

// Programın top-level `use` modüllerini interpreter'a yükle. Hepsi stdlib ise true
// (CLI-VM güvenli); dış/paket modül varsa false (caller tree-walk'a düşer — semantik korunur).
static bool preload_uses_for_vm(look::Interpreter& interp, const look::Program& prog) {
    for (auto& s : prog.statements) {
        if (auto* u = dynamic_cast<const look::UseStatement*>(s.get())) {
            if (!interp.load_stdlib_module(u->module_name)) return false;
        }
    }
    return true;
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
                              << "Örnek: lk module install github.com/codlook/look-modules/jwt\n";
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
            std::string pkg;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--verbose" || arg == "-v") verbose = true;
                else if (pkg.empty()) pkg = arg;
            }
            if (pkg.empty()) return look::cmd_install_all(verbose);
            return look::cmd_install(pkg, verbose);
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

        // Parse basarili — --check modunda: tanimsiz cagri semantik kontrolu,
        // sonra calistirmadan cik (yan etki yok).
        if (check_only) {
            std::string bad; int bl = 0, bc = 0;
            if (find_undefined_call(*program, bad, bl, bc)) {
                std::cout << "CHECK " << (bl > 0 ? bl : 1) << " " << (bc > 0 ? bc : 1)
                          << " Undefined function: " << bad << "\n";
                return 1;
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
            // C9: CLI top-level artık DEFAULT olarak bytecode VM'de çalışır (~37×).
            // Kaçış kapağı: LOOK_CLI_VM=0 → tree-walk (sorun görülürse anında geri dönüş).
            //
            // Güvenlik: VM yoluna ancak tüm `use`'lar stdlib ise VE compile başarılıysa
            // girilir — İKİSİ DE EXECUTION ÖNCESİ. Dış/paket modül veya compile hatası →
            // sessizce tree-walk. Çıktı taahhüt edildikten SONRA fallback YOK (çift çıktı
            // riski yok).
            //
            // Default'a alınabilmesinin dayanağı: 3 motor differential (19 kategori,
            // tree-walk == CLI-VM == web-VM) + CLI'ye özgü yüzey (print/write bayt-bayt,
            // exit kodu) + hata raporu artık tree-walk formatında (File/Line — bkz. aşağı).
            // Bilinen tek fark: hata çıktısında Column yok (VM sütun tablosu tutmuyor).
            bool ran_vm = false;
            const char* cvm = std::getenv("LOOK_CLI_VM");
            const bool want_vm = !(cvm && cvm[0] == '0');   // default AÇIK; sadece "0" kapatır
            if (want_vm && preload_uses_for_vm(interpreter, *program)) {
                look::CompiledProgram compiled;
                bool compiled_ok = true;
                try { compiled = look::Compiler::compile(*program); }
                catch (...) { compiled_ok = false; }   // compile hatası → tree-walk
                // Builtin OLMAYAN "mod::fn" (ör. cache::keys, template::escape) → o çağrı
                // RUNTIME'da "Not callable" fırlatır ve CLI-VM'de fallback YOKTUR
                // (web'de route interpreter'a düşüp kurtulur). Bayrak varsa daha en baştan
                // tree-walk — böylece VM default'u ÇALIŞAN script'leri kırmaz.
                if (compiled_ok && compiled.uses_non_builtin_module_fn) compiled_ok = false;
                if (compiled_ok) {
                    auto cli_builtins = build_cli_builtins(interpreter, std::cout);
                    look::VM::SharedState sh;
                    sh.builtins = &cli_builtins;
                    look::VM vm(sh, std::cout);
                    vm.set_web_context(&web_ctx);
                    // Hata raporu tree-walk ile aynı biçimde olmalı: yakalanmamış hatada
                    // VM'in kaydettiği satırı (proto.lines — compile_stmt doldurur) alıp
                    // LookRuntimeError'a sarıyoruz. Aksi halde CLI-VM sadece "Error: msg"
                    // basardı; tree-walk "Runtime Error ... File/Line" veriyor → VM'i
                    // default yapmak hata kalitesini DÜŞÜRÜRDÜ ("developer için çalış").
                    // ExitException/RouteMatched kontrol akışıdır — sarılmaz, geçer.
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
                    ran_vm = true;
                }
            }
            if (!ran_vm)
                interpreter.interpret(*program);
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

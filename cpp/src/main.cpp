#include "look/read_stream.h"   // read_stream — dosyayı tek seferde oku
#include "look/lexer.h"
#include "look/parser.h"
#include "look/capture_check.h"
#include "look/arg_check.h"
#include "look/format_src.h"   // lk fmt
#include "look/runtime_host.h"
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
#include "look/cli_runtime.h"   // build_cli_builtins / wire_cli_callbacks — lk ve lk test aynı ortam
#include "look/timer.h"         // TimerManager — komut satırında timer:: VM closure'ı kurar
#include "look/vm_closure_runner.h" // vm_closure_runner — jobs::run işleyicilerini VM'de koşturur
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
    return look::read_stream(input);
}

// ── Semantik kontrol (yalniz --check) — tanimsiz bare fonksiyon cagrilari ──────
// Guvenli/yanlis-pozitifsiz olmak icin: yalniz CallExpression'in callee'si bare
// ($'siz, ::'siz) Variable olanlari; gecerli kume = dilin bare yerlesikleri
// (derleyicinin ozel ele aldiklari dahil) + builtin_names() + dosyadaki fn/struct/const adlari. Dosyada
// `use` importu varsa (cross-file isimler bilinemez) hic kontrol edilmez.
namespace {
using namespace look;

void walk_expr(const Expression* e, const std::function<void(const Expression*)>& v);
void walk_block(const BlockStatement* b, const std::function<void(const Expression*)>& v);

void walk_stmt(const Statement* s, const std::function<void(const Expression*)>& v) {
    if (!s) return;
    if (auto* x = node_as<ExpressionStatement>(s)) walk_expr(x->expression.get(), v);
    else if (auto* x = node_as<PrintStatement>(s)) { for (auto& e : x->expressions) walk_expr(e.get(), v); }
    else if (auto* x = node_as<WriteStatement>(s)) { for (auto& e : x->expressions) walk_expr(e.get(), v); }
    else if (auto* x = node_as<ReturnStatement>(s)) walk_expr(x->expression.get(), v);
    else if (auto* x = node_as<BlockStatement>(s)) walk_block(x, v);
    else if (auto* x = node_as<IfStatement>(s)) { walk_expr(x->condition.get(), v); walk_block(x->then_branch.get(), v); walk_block(x->else_branch.get(), v); }
    else if (auto* x = node_as<WhileStatement>(s)) { walk_expr(x->condition.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = node_as<ForStatement>(s)) { walk_stmt(x->init.get(), v); walk_expr(x->condition.get(), v); walk_expr(x->post.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = node_as<ForeachStatement>(s)) { walk_expr(x->iterable.get(), v); walk_block(x->body.get(), v); }
    else if (auto* x = node_as<TryCatchStatement>(s)) { walk_block(x->try_block.get(), v); walk_block(x->catch_block.get(), v); walk_block(x->finally_block.get(), v); }
    else if (auto* x = node_as<SwitchStatement>(s)) { walk_expr(x->subject.get(), v); for (auto& c : x->cases) { for (auto& val : c.values) walk_expr(val.get(), v); for (auto& st : c.body) walk_stmt(st.get(), v); } }
    else if (auto* x = node_as<FunctionDeclaration>(s)) walk_block(x->body.get(), v);
}

void walk_block(const BlockStatement* b, const std::function<void(const Expression*)>& v) {
    if (!b) return;
    for (auto& s : b->statements) walk_stmt(s.get(), v);
}

void walk_expr(const Expression* e, const std::function<void(const Expression*)>& v) {
    if (!e) return;
    v(e);
    if (auto* x = node_as<ArrayLiteral>(e)) { for (auto& el : x->elements) walk_expr(el.get(), v); }
    else if (auto* x = node_as<AssocArrayLiteral>(e)) { for (auto& p : x->pairs) { walk_expr(p.first.get(), v); walk_expr(p.second.get(), v); } }
    else if (auto* x = node_as<FunctionExpression>(e)) walk_block(x->body.get(), v);
    else if (auto* x = node_as<IndexExpression>(e)) { walk_expr(x->object.get(), v); walk_expr(x->index.get(), v); }
    else if (auto* x = node_as<UnaryExpression>(e)) walk_expr(x->right.get(), v);
    else if (auto* x = node_as<BinaryExpression>(e)) { walk_expr(x->left.get(), v); walk_expr(x->right.get(), v); }
    else if (auto* x = node_as<AssignmentExpression>(e)) { walk_expr(x->index.get(), v); walk_expr(x->value.get(), v); }
    else if (auto* x = node_as<CallExpression>(e)) { walk_expr(x->callee.get(), v); for (auto& a : x->arguments) walk_expr(a.get(), v); }
    else if (auto* x = node_as<TernaryExpression>(e)) { walk_expr(x->condition.get(), v); walk_expr(x->then_expr.get(), v); walk_expr(x->else_expr.get(), v); }
    else if (auto* x = node_as<MemberAccessExpression>(e)) walk_expr(x->object.get(), v);
    else if (auto* x = node_as<StructLiteralExpression>(e)) { for (auto& f : x->fields) walk_expr(f.second.get(), v); }
}

void collect_decls(const Statement* s, std::set<std::string>& d) {
    if (!s) return;
    if (auto* f = node_as<FunctionDeclaration>(s)) { d.insert(f->name); if (f->body) for (auto& st : f->body->statements) collect_decls(st.get(), d); }
    else if (auto* st = node_as<StructDeclaration>(s)) d.insert(st->name);
    else if (auto* cb = node_as<ConstBlock>(s)) { for (auto& it : cb->items) d.insert(it.name); }
    else if (auto* b = node_as<BlockStatement>(s)) { for (auto& x : b->statements) collect_decls(x.get(), d); }
    else if (auto* i = node_as<IfStatement>(s)) { if (i->then_branch) for (auto& x : i->then_branch->statements) collect_decls(x.get(), d); if (i->else_branch) for (auto& x : i->else_branch->statements) collect_decls(x.get(), d); }
    else if (auto* w = node_as<WhileStatement>(s)) { if (w->body) for (auto& x : w->body->statements) collect_decls(x.get(), d); }
    else if (auto* fr = node_as<ForStatement>(s)) { if (fr->body) for (auto& x : fr->body->statements) collect_decls(x.get(), d); }
    else if (auto* fe = node_as<ForeachStatement>(s)) { if (fe->body) for (auto& x : fe->body->statements) collect_decls(x.get(), d); }
    else if (auto* t = node_as<TryCatchStatement>(s)) { if (t->try_block) for (auto& x : t->try_block->statements) collect_decls(x.get(), d); if (t->catch_block) for (auto& x : t->catch_block->statements) collect_decls(x.get(), d); if (t->finally_block) for (auto& x : t->finally_block->statements) collect_decls(x.get(), d); }
    else if (auto* sw = node_as<SwitchStatement>(s)) { for (auto& c : sw->cases) for (auto& st : c.body) collect_decls(st.get(), d); }
}

bool find_undefined_call(const Program& prog, std::string& name, int& line, int& col) {
    for (auto& s : prog.statements)
        if (node_as<UseStatement>(s.get()) || node_as<UseFileStatement>(s.get()))
            return false;   // import var → cross-file isimler bilinemez, kontrol etme

    // Gecerli bare cagri isimleri: dilin bare yerlesikleri +
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
        auto* call = node_as<CallExpression>(e);
        if (!call) return;
        auto* var = node_as<Variable>(call->callee.get());
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
// LOOK 2: betik bitince süreç biter (Go/Rust modeli: programın ömrünü üst düzey belirler, beklemek
// açıkça yazılır). Sessizce değil: kurulu kalan zamanlayıcılar ve bitmemiş parallel() görevleri
// söylenir, beklemek için ne yazılacağıyla. (Eskiden parallel() görevleri örtük 5 sn bekleniyor,
// zamanlayıcılar sessizce kayboluyordu — iki ayrı kural; artık tek kural.)
static void warn_pending_timers();
static int finish_script(int code) {
    const int running = look::task_active();
    if (running > 0)
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "parallel",
            std::to_string(running) + (running == 1 ? " parallel() task was" : " parallel() tasks were") +
            " still running when the script ended and stopped with it. To wait for them, call parallel::wait() before the script ends.");
    warn_pending_timers();
    if (running > 0) {
        // Çalışan görevler süreç belleğini kullanırken statik yıkıcılar çalışmasın: doğrudan çık.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(code);
    }
    return code;
}
static void warn_pending_timers() {
    if (!look::TimerManager::exists()) return;   // zamanlayıcı hiç kurulmadı — iş parçacığı başlatma
    const auto p = look::TimerManager::instance().pending();
    if (p.once > 0)
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "timer",
            std::to_string(p.once) + (p.once == 1 ? " timer::after callback never ran" : " timer::after callbacks never ran") +
            ": the script ended first. To wait for them, call timer::wait() before the script ends.");
    if (p.repeating > 0)
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "timer",
            std::to_string(p.repeating) + (p.repeating == 1 ? " timer::every timer was" : " timer::every timers were") +
            " still set when the script ended and stopped with it. Stop one with timer::cancel($id) when it is no longer needed.");
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
            look::RuntimeHost rint;
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
            // LOOK 2 geçiş denetimi: adlı fonksiyona yanlış sayıda argümanla çağrı (1.0.x'te iki
            // motor ayrışıyordu; kural kesinleşmeden önce sayım için).
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

        look::RuntimeHost interpreter;
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
            look::CompiledProgram compiled;
            try {
                compiled = look::Compiler::compile(*program, script_dir.string());
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n  File: " << filename << std::endl;
                return 1;
            }
            // Yalnız `use` edilen stdlib modülleri açılır — derleyicinin topladığı liste (web ile aynı;
            // dahil edilen dosyalarınkiler dahil). Paket modülü stdlib değildir: derleyici derler.
            for (const auto& m : compiled.use_modules) interpreter.load_stdlib_module(m);
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
            cli_builtins = look::build_cli_builtins(interpreter, std::cout);
            sh.builtins = &cli_builtins;
            auto callbacks = std::make_shared<look::CliCallbacks>();
            callbacks->shared = &sh;
            look::wire_cli_callbacks(cli_builtins, interpreter, callbacks);
            look::VM vm(sh, std::cout);
            vm.set_web_context(&web_ctx);
            callbacks->main_vm = &vm;
            struct MainVmGone { std::shared_ptr<look::CliCallbacks> c; ~MainVmGone() { c->main_vm = nullptr; } } main_vm_gone{callbacks};
            // Yakalanmamış hata "Runtime Error ... File/Line" biçiminde raporlanır: VM'in kaydettiği satır
            // (proto.lines) LookRuntimeError'a sarılır. ExitException/RouteMatched kontrol akışıdır.
            try {
                vm.execute(compiled);
            } catch (const look::ExitException&)         { throw; }
              catch (const look::RouteMatchedException&) { throw; }
              catch (const look::LookRuntimeError&)      { throw; }  // zaten konumlu
              catch (const std::exception& ex) {
                look::SourceLocation loc;
                // Dahil edilen dosyadaki hata o dosyanın adıyla (ESKİ HATA: ana betiğin adı + dahil
                // edilen dosyanın satırı — "main.lk, Line: 7" beş satırlık bir main.lk için).
                const std::string& ef = vm.last_error_file();
                loc.file = ef.empty() ? filename : (std::filesystem::path(filename).parent_path() / ef).generic_string();
                loc.line = vm.last_error_line();
                throw look::LookRuntimeError(ex.what(), loc, {});
            }
        } catch (const look::RouteMatchedException&) {
            // route() flow control — normal
        } catch (const look::ExitException& e) {
            // exit()/die() — NORMAL sonlanma, hata değil. Yakalanmıyordu: dıştaki genel
            // catch'e düşüp "Error: exit" basıyor ve kodu 1 döndürüyordu (exit(3) → 1).
            // cgi_main/fcgi_main zaten yakalıyordu — CLI eksikti.
            return finish_script(e.code());
        } catch (const look::LookRuntimeError& e) {
            auto err = e;
            if (err.location.file.empty()) err.location.file = filename;
            std::cerr << err.format() << std::endl;
            return finish_script(1);
        }
        return finish_script(0);
    } catch (const look::LookRuntimeError& e) {
        std::cerr << e.format() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

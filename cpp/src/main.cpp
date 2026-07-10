#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/http_client.h"
#include "look/test_runner.h"
#include "look/repl.h"
#include "look/web.h"
#include "look/installer.h"
#include "look/parallel_runtime.h"
#include "look/ast.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

#define LOOK_VERSION "1.0.0"

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
        "abs","before_route","bool","boolval","chan_size","channel","close","config","count",
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
} // namespace

int main(int argc, char* argv[]) {
    look::configure_system_ca_bundle();  // statik OpenSSL: sistem CA'sını bul (TLS'ten önce)
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
                      << " (" << LOOK_PLATFORM << "/" << LOOK_ARCH << ")"
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

        // ── lk module <sub> ──────────────────────────────────────────────────
        if (cmd == "module") {
            if (argc < 3) {
                std::cout << "Kullanım:\n"
                          << "  lk module install <github.com/user/repo>  — modül yükle\n"
                          << "  lk module list                             — resmi modülleri listele\n";
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
                    std::cerr << "Hata: GitHub linki gerekli.\n"
                              << "Örnek: lk module install github.com/codlook/look-modules/jwt\n";
                    return 1;
                }
                return look::cmd_module_install(pkg_url, verbose);
            }
            if (sub == "list") {
                return look::cmd_module_list();
            }
            std::cerr << "Bilinmeyen alt komut: " << sub << "\n";
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
        } else {
            source = read_file(cmd);
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
            interpreter.interpret(*program);
        } catch (const look::RouteMatchedException&) {
            // route() flow control — normal
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

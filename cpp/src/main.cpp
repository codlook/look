#include "look/read_stream.h"   // read_stream — read the file in one go
#include "look/stack_guard.h"
#include "look/ast_spine.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/capture_check.h"
#include "look/arg_check.h"
#include "look/format_src.h"   // lk fmt
#include "look/fix_src.h"      // lk fix
#include "look/runtime_host.h"
#include "look/array_count.h"
#include "look/http_client.h"
#include "look/runtime_init.h"    // runtime_init (CA probe + SQLite init, process start)
#include "look/mem_budget.h"      // command line: process limit only if LOOK_MEMORY_LIMIT is given
#include "look/test_runner.h"
#include "look/repl.h"
#include "look/web.h"
#include "look/installer.h"
#include "look/parallel_runtime.h"
#include "look/ast.h"
#include "look/builtins.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/cli_runtime.h"   // build_cli_builtins / wire_cli_callbacks — lk and lk test share the same environment
#include "look/timer.h"         // TimerManager — sets up the timer:: VM closure on the command line
#include "look/vm_closure_runner.h" // vm_closure_runner — runs jobs::run handlers in the VM
#include <memory>
#include <unordered_map>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <streambuf>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normally CMake supplies the compile-def (single source)
#endif
#ifndef LOOK_BUILD          // if there is no CMake stamp (raw build), a safe default
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
    std::cout << "  lk fmt <file.lk>...              — format files in place (no files: stdin to stdout)\n";
    std::cout << "  lk fmt --check <file.lk>...      — list unformatted files; exit 1 if any\n";
    std::cout << "  lk fix <file.lk>...              — rewrite push()/pop()/array::pop() in place (no files: stdin to stdout)\n";
    std::cout << "  lk fix --check <file.lk>...      — list what would be rewritten; exit 1 if anything would\n";
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

// ── Semantic check (--check only) — undefined bare function calls ──────
// To be safe / free of false positives: only those whose CallExpression callee is a bare
// Variable (no $, no ::); the valid set = the language's bare built-ins
// (including those the compiler handles specially) + builtin_names() + the fn/struct/const names in the file. If the file has a
// `use` import (cross-file names cannot be known) nothing is checked at all.
namespace {
using namespace look;

void walk_expr(const Expression* e, const std::function<void(const Expression*)>& v);
void walk_block(const BlockStatement* b, const std::function<void(const Expression*)>& v);

void walk_stmt(const Statement* s, const std::function<void(const Expression*)>& v) {
    if (s) look_ast_guard(s->loc.line, s->loc.column);   // deep tree: an error instead of a crash (stack_guard.h)
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
    if (e) look_ast_guard(e->loc.line, e->loc.column);   // deep tree: an error instead of a crash (stack_guard.h)
    if (!e) return;
    v(e);
    if (auto* x = node_as<ArrayLiteral>(e)) { for (auto& el : x->elements) walk_expr(el.get(), v); }
    else if (auto* x = node_as<AssocArrayLiteral>(e)) { for (auto& p : x->pairs) { walk_expr(p.first.get(), v); walk_expr(p.second.get(), v); } }
    else if (auto* x = node_as<FunctionExpression>(e)) walk_block(x->body.get(), v);
    else if (auto* x = node_as<IndexExpression>(e)) { walk_expr(x->object.get(), v); walk_expr(x->index.get(), v); }
    else if (auto* x = node_as<UnaryExpression>(e)) walk_expr(x->right.get(), v);
    else if (auto* x = node_as<BinaryExpression>(e)) {   // left spine with a loop (look/ast_spine.h)
        std::vector<const BinaryExpression*> sp; const Expression* lm = binary_left_spine(x, sp);
        for (size_t k = 1; k < sp.size(); ++k) v(sp[k]);   // inner nodes, in the order of the recursion
        walk_expr(lm, v);
        for (size_t k = sp.size(); k-- > 0;) walk_expr(sp[k]->right.get(), v);
    }
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
            return false;   // there is an import -> cross-file names cannot be known, do not check

    // Valid bare call names: the language's bare built-ins +
    // bare entries of builtin_names() + generous extras (to avoid false positives).
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
// LOOK 2: the process ends when the script ends (Go/Rust model: the top level determines the program's lifetime, waiting
// is written explicitly). Not silently: timers left set up and unfinished parallel() tasks are
// reported, together with what to write in order to wait. (Previously parallel() tasks were implicitly waited for 5 s and
// timers silently vanished — two separate rules; now a single rule.)
static void warn_pending_timers();
static int finish_script(int code) {
    look::chan_deadlock_main(false);   // script finished: waiting tasks are no longer a deadlock
    // Only tasks that STILL HAVE WORK are counted: a running task makes progress until it returns or starts
    // an operation that leaves a trace outside (no time, no waiting — parallel_runtime.h). A task that has only its
    // return left after send() is no longer counted as "still running" (previously a false warning on every run).
    const int running = look::task_settle_for_exit();
    if (running > 0)
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "parallel",
            std::to_string(running) + (running == 1 ? " parallel() task was" : " parallel() tasks were") +
            " still running when the script ended and stopped with it. To wait for them, call parallel::wait() before the script ends.");
    warn_pending_timers();
    if (running > 0) {
        // Static destructors must not run while running tasks use process memory: exit directly.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(code);
    }
    return code;
}
static void warn_pending_timers() {
    if (!look::TimerManager::exists()) return;   // no timer was ever set up — thread start
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


#ifndef _WIN32
// runtime_init ignores SIGPIPE (a closed DB socket must not kill the process). But in usage like `lk x.lk | head`
// the script must terminate as before when the output pipe closes — otherwise print silently
// fails and an infinite loop runs forever. This wrapper sits in front of std::cout/cerr
// (it does not change buffering); if the underlying write returns EPIPE, the process
// terminates with the default SIGPIPE — identical to the behavior up to 1.0.34, for stdout/stderr only.
struct PipeGuard : std::streambuf {
    std::streambuf* inner;
    explicit PipeGuard(std::streambuf* in) : inner(in) {}
    static void die_if_epipe() {
        if (errno == EPIPE) { std::signal(SIGPIPE, SIG_DFL); std::raise(SIGPIPE); }
    }
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        if (inner->sputc((char)c) == traits_type::eof()) { die_if_epipe(); return traits_type::eof(); }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize w = inner->sputn(s, n);
        if (w < n) die_if_epipe();
        return w;
    }
    int sync() override {
        int r = inner->pubsync();
        if (r != 0) die_if_epipe();
        return r;
    }
};
#endif

} // namespace

int main(int argc, char* argv[]) {
    look::runtime_init();  // all process-start init (CA probe + one-time SQLite init) — shared by the 3 entry points
    // Memory (look/mem_budget.h): no default limit on the command line (like Go); a process limit only if LOOK_MEMORY_LIMIT
    // is given explicitly. The request budget (256 MiB) belongs to web requests only.
    {
        std::string why;
        if (!look::mem_budget_configure_cli(why)) {
            std::cerr << "Error: " << why << std::endl;
            return 1;
        }
    }
#ifndef _WIN32
    // Never destroyed: at exit std::cout is flushed through this wrapper (if it were static it would be
    // destroyed first, and then ios_base::Init would write to a dead object).
    std::cout.rdbuf(new PipeGuard(std::cout.rdbuf()));
    std::cerr.rdbuf(new PipeGuard(std::cerr.rdbuf()));
#endif
    try {
        if (argc < 2) { print_usage(); return 1; }

        std::string cmd = argv[1];

        // ── lk --check <file> : only parse (DO NOT RUN), report errors
        //    in a machine-readable form. For editor diagnostics — no side effects.
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

        // ── lk fmt <file...> [--check] : bring the source to the canonical form (gofmt model) ──
        // --check -> CI mode: exit 1 if a file is unformatted, does NOT write. No files -> stdin/stdout.
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

        // ── lk fix <file...> [--check] : push/pop/array::pop -> new syntax (look/fix_src.h) ──
        if (cmd == "fix") {
            bool fix_check = false;
            std::vector<std::string> files;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--check") fix_check = true;
                else files.push_back(arg);
            }
            return look::run_fix(files, fix_check);
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
            // `lk file.lk <args...>` — the words after the script go to args().
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

        // Parse succeeded — in --check mode: semantic check for undefined calls,
        // then exit without running (no side effects).
        if (check_only) {
            std::string bad; int bl = 0, bc = 0;
            if (find_undefined_call(*program, bad, bl, bc)) {
                std::cout << "CHECK " << (bl > 0 ? bl : 1) << " " << (bc > 0 ? bc : 1)
                          << " Undefined function: " << bad << "\n";
                return 1;
            }
            // LOOK 2 transition check: places the closure-capture rule would break (a warning for now;
            // exit code does not change, the editor extension only looks at the "CHECK" line).
            for (const auto& w : look::check_captures(*program))
                std::cout << "WARN " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                          << " [" << w.kind << "] " << w.message << "\n";
            // LOOK 2 transition check: a call to a named function with the wrong number of arguments (in 1.0.x the two
            // engines diverged; counted before the rule is finalized).
            // push($l, $x) -> $l = array::push($l, $x); pop($l) / array::pop($l) -> $l[-1] +
            // $l = array::slice($l, 0, -1): a warning for one release (then an error). `lk fix` rewrites it.
            for (const auto& w : parser.warnings())
                if (w.kind == "push-deprecated" || w.kind == "pop-deprecated" || w.kind == "array-pop-deprecated" ||
                    w.kind == "use-array-missing")
                    std::cout << "WARN " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                              << " [" << w.kind << "] " << w.message << "\n";
            for (const auto& w : look::check_arg_counts(*program))
                std::cout << "WARN " << (w.line > 0 ? w.line : 1) << " " << (w.column > 0 ? w.column : 1)
                          << " [arg-count] " << w.message << "\n";
            // LOOK 2: a function that writes to a parameter without returning it — an ERROR here (for CI); a warning at load time.
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
            // Can the VM compile this file? In LOOK 2 a program that cannot be compiled does NOT RUN (command line and
            // web) — said here by name, as an error. Nothing is run.
            // The base directory is given as on the web path: files included with `use "file.lk"` are compiled too.
            try {
                auto checked = look::Compiler::compile(*program, std::filesystem::absolute(filename).parent_path().string(), filename);
                // A nonexistent module function: `string::uper(...)`. Reported only for CORE modules
                // (modules whose name appears in the built-in table) — package module functions are not known here.
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
                // In a sub-file checked on its own, `use "../x.lk"` cannot be resolved without knowing
                // the application root (the root is the entry file's directory) — this is not a VM gap, not reported.
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
            // LOOK 2 single engine: the command line runs ONLY in the VM. Previously a compile error, a package
            // module, timer::/jobs::worker or a mod::fn missing from the VM silently (and then with
            // a warning) dropped the script to a ~40x slower tree-walk; LOOK_CLI_VM=0 selected it.
            // Now: a program that cannot be compiled does NOT RUN (the reason is printed, exit 1); LOOK_CLI_VM=0 and
            // LOOK_BYTECODE=0 are ignored with a warning (same as the web server).
            for (const char* var : {"LOOK_CLI_VM", "LOOK_BYTECODE"}) {
                const char* v = std::getenv(var);
                if (v && v[0] == '0')
                    look::Logger::instance().log(look::LogLevel::LOG_WARN, "VM",
                        std::string(var) + "=0 is ignored: LOOK runs only on the VM.");
            }
            const std::filesystem::path script_dir = std::filesystem::absolute(filename).parent_path();
            look::CompiledProgram compiled;
            try {
                compiled = look::Compiler::compile(*program, script_dir.string(), filename);
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n  File: " << filename << std::endl;
                return 1;
            }
            // Only `use`d stdlib modules are opened — the list collected by the compiler (same as the web;
            // including those of included files). A package module is not stdlib: the compiler compiles it.
            for (const auto& m : compiled.use_modules) interpreter.load_stdlib_module(m);
            // A mod::fn name with no counterpart in the VM is a typo (same as the web's "does not exist"
            // rule) — reported without running.
            if (compiled.uses_non_builtin_module_fn) {
                for (const auto& n : compiled.non_builtin_module_fns)
                    std::cerr << "Error: " << n << "() does not exist — check the spelling of the module and the function.\n";
                std::cerr << "  File: " << filename << std::endl;
                return 1;
            }
            // The built-in table and shared state are PROCESS-LIFETIME: a timer can fire after the script has ended too
            // (while parallel tasks are being waited for).
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
            // An uncaught error is reported in the "Runtime Error ... File/Line" form: the line the VM recorded
            // (proto.lines) is wrapped into a LookRuntimeError. ExitException/RouteMatched are control flow.
            look::chan_deadlock_main(true);   // channel deadlock: script + tasks (parallel_runtime.h)
            try {
                vm.execute(compiled);
            } catch (const look::ExitException&)         { throw; }
              catch (const look::RouteMatchedException&) { throw; }
              catch (const look::LookRuntimeError& e) {
                // an error::new() value arrives without a position: complete the line and call chain from the VM.
                if (!e.stack.empty() || vm.last_error_stack().empty()) throw;
                auto err = e;
                if (err.location.line <= 0) {
                    const std::string& ef = vm.last_error_file();
                    err.location.file = ef.empty() ? filename : (std::filesystem::path(filename).parent_path() / ef).generic_string();
                    err.location.line = vm.last_error_line();
                }
                err.stack = vm.last_error_stack();
                throw err;
            }
              catch (const std::exception& ex) {
                look::SourceLocation loc;
                // An error in an included file is reported with that file's name (OLD BUG: the main script's name + the included
                // file's line — "main.lk, Line: 7" for a five-line main.lk).
                const std::string& ef = vm.last_error_file();
                loc.file = ef.empty() ? filename : (std::filesystem::path(filename).parent_path() / ef).generic_string();
                loc.line = vm.last_error_line();
                throw look::LookRuntimeError(ex.what(), loc, vm.last_error_stack());
            }
        } catch (const look::RouteMatchedException&) {
            // route() flow control — normal
        } catch (const look::ExitException& e) {
            // exit()/die() — NORMAL termination, not an error. It was not being caught: it fell into the outer generic
            // catch, printed "Error: exit" and returned code 1 (exit(3) -> 1).
            // cgi_main/fcgi_main already caught it — the CLI was missing it.
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

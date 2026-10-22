// repl.cpp — `look repl` interactive session
#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normalde CMake compile-def sağlar (tek kaynak)
#endif
#include "look/repl.h"
#include "look/runtime_host.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/web.h"
#include "look/ast.h"
#include "look/cli_runtime.h"   // build_cli_builtins / wire_cli_callbacks — lk ile aynı VM ortamı
#include "look/compiler.h"
#include "look/vm.h"
#include <filesystem>
#include <memory>
#include <set>
#include <unordered_map>

extern "C" {
#include "linenoise/linenoise.h"
}

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace look {

// ── ANSI colors (disabled on non-TTY / Windows without VT) ───────────────
#ifdef _WIN32
#  include <windows.h>
static bool vt_enabled = false;
static void enable_vt() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        vt_enabled = true;
}
#else
static bool vt_enabled = true;
static void enable_vt() {}
#endif

static std::string colored(const char* code, const std::string& s) {
    if (!vt_enabled) return s;
    return std::string("\033[") + code + "m" + s + "\033[0m";
}
static std::string green(const std::string& s)  { return colored("32", s); }
static std::string red(const std::string& s)    { return colored("31", s); }
static std::string cyan(const std::string& s)   { return colored("36", s); }
static std::string yellow(const std::string& s) { return colored("33", s); }
static std::string gray(const std::string& s)   { return colored("90", s); }

// ── Open-delimiter counter — determines if input is complete ─────────────
// Returns > 0 if there are unclosed {} [] () — the REPL keeps collecting
// lines until all are balanced. Eskiden yalnız {} sayılırdı; çok satırlı
// paren/bracket ifadeleri (foo(\n...\n) veya dizi literali) ilk satırda erken
// çalıştırılıp parse hatası veriyordu. Parantez/köşeli de aynı mantıkla sayılır.
static int open_braces(const std::string& src) {
    int depth = 0;
    bool in_str = false;
    char str_ch = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == str_ch) in_str = false;
        } else if (c == '"' || c == '\'') {
            in_str = true; str_ch = c;
        } else if (c == '#') {
            // Line comment — skip to end
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (c == '{' || c == '[' || c == '(') depth++;
        else if (c == '}' || c == ']' || c == ')') depth--;
    }
    return depth;
}

// ── Pretty-print a Value for REPL output ─────────────────────────────────
static std::string repl_display(const Value& v) {
    switch (v.type()) {
    case Value::NONE:   return gray("null");
    case Value::BOOL:   return v.as_bool() ? green("true") : red("false");
    case Value::INT:    return cyan(std::to_string(v.to_int()));
    case Value::FLOAT: {
        std::ostringstream oss;
        oss << v.to_float();
        return cyan(oss.str());
    }
    case Value::STRING:
        return "\"" + yellow(v.as_string()) + "\"";
    case Value::ARRAY: {
        auto& arr = *v.as_array();
        if (arr.empty()) return "[]";
        // Check if assoc (has __assoc__ sentinel)
        bool is_assoc = false;
        for (auto& e : arr)
            if (e.type() == Value::STRING && e.as_string() == "__assoc__") { is_assoc = true; break; }
        if (is_assoc) {
            std::ostringstream oss;
            oss << "{";
            bool first = true, skip_next = false;
            bool found_sentinel = false;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!found_sentinel && arr[i].type() == Value::STRING && arr[i].as_string() == "__assoc__") {
                    found_sentinel = true; ++i; continue;
                }
                if (!first) oss << ", ";
                if (i < arr.size()) {
                    oss << "\"" << arr[i].to_string() << "\": ";
                    ++i;
                    if (i < arr.size()) oss << repl_display(arr[i]);
                }
                first = false;
            }
            oss << "}";
            return oss.str();
        }
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) oss << ", ";
            oss << repl_display(arr[i]);
        }
        oss << "]";
        return oss.str();
    }
    case Value::BYTECODE_FN:
        return gray("<function>");
    default:
        return gray(v.to_string());
    }
}

// ── :vars — list all globals defined by user ──────────────────────────────
static void print_vars(const std::unordered_map<std::string, Value>& globals) {
    std::vector<std::string> vars;
    for (const auto& [name, v] : globals) {
        if (name.empty() || name.rfind("__", 0) == 0) continue;   // derleyicinin iç adları (__sdef: …)
        // Değişkenler "$ad", adlı fonksiyonlar "ad" olarak tutulur → fonksiyon "ad()" gösterilir.
        vars.push_back(name[0] == '$' ? name : name + "()");
    }
    if (vars.empty()) { std::cout << gray("  (no variables yet)") << "\n"; return; }
    std::sort(vars.begin(), vars.end());
    for (auto& name : vars)
        std::cout << "  " << name << "\n";
}

// ── :help ─────────────────────────────────────────────────────────────────
static void print_help() {
    std::cout << "\n" << cyan("LOOK REPL Commands:") << "\n";
    std::cout << "  :help     — this help\n";
    std::cout << "  :vars     — defined variables\n";
    std::cout << "  :clear    — clear the screen\n";
    std::cout << "  :exit     — exit (or Ctrl+C / Ctrl+D)\n\n";
    std::cout << cyan("Tips:") << "\n";
    std::cout << "  The result is shown automatically — you don't need to write print()\n";
    std::cout << "  Multi-line: a block opened with { waits until it is closed with }\n";
    std::cout << "  Arrow keys: command history (up/down), cursor (left/right)\n\n";
}

// ── Wrap source as expression-or-statement ────────────────────────────────
// If the input looks like an expression (not an assignment, route, etc.),
// we wrap it in a special "eval print" form by compiling as expression.
// Strategy: try to parse as expression first; if it succeeds and produces
// a non-null value, display it. Otherwise interpret as statement.

static bool is_colon_command(const std::string& s) {
    return !s.empty() && s[0] == ':';
}

static bool handle_colon_command(const std::string& s, const std::unordered_map<std::string, Value>& globals) {
    if (s == ":exit" || s == ":quit" || s == ":q") return false;  // signal exit
    if (s == ":help" || s == ":h") { print_help(); return true; }
    if (s == ":vars") { print_vars(globals); return true; }
    if (s == ":clear") { linenoiseClearScreen(); return true; }
    // :time <expr> — placeholder (future)
    if (s.substr(0,5) == ":time") {
        std::cout << yellow("  :time not yet implemented\n");
        return true;
    }
    std::cout << red("  Unknown command: ") << s << "\n";
    std::cout << gray("  Type :help to see the commands\n");
    return true;
}

// ── Main REPL loop ────────────────────────────────────────────────────────
int run_repl() {
    enable_vt();
    linenoiseHistorySetMaxLen(200);

    // Try to load history from ~/.look_history
    const char* hist_file = ".look_history";
    linenoiseHistoryLoad(hist_file);

    std::cout << cyan("LOOK v" LOOK_VERSION " REPL") << "\n";
    std::cout << gray("Type :exit or Ctrl+C to quit") << "\n\n";

    // LOOK 2 (tek motor, adım 6): REPL VM'de koşar. Her satır ayrı derlenir ve YENİ bir VM'de
    // çalışır; önceki satırların global'leri (değişkenler, fonksiyonlar) aktarılır — bir satır
    // hatayla bitse de o ana kadar atananlar kalır. Yerleşik tablo komut satırıyla aynı (cli_runtime).
    RuntimeHost interp;   // modül deposu (ModuleHost) — LOOK kodu çalıştırmaz
    WebContext  web_ctx;
    web_ctx.method = "GET";
    web_ctx.path   = "/";
    interp.set_web_context(&web_ctx);
    interp.set_file("<repl>");
    // Standart modüller baştan açık — REPL'de `use X` yazmak gerekmez (eskisi gibi).
    for (const char* m : {"math", "string", "array", "type", "date", "file", "auth", "validator",
                          "html", "http", "template"})
        interp.load_stdlib_module(m);

    static std::vector<BuiltinFn> builtins;   // zamanlayıcı geri çağırmaları satırdan uzun yaşayabilir
    static VM::SharedState sh;
    sh.builtins = &builtins;
    auto callbacks = std::make_shared<CliCallbacks>();
    callbacks->shared = &sh;
    std::unordered_map<std::string, Value> globals;   // satırlar arası durum

    std::string pending;  // accumulated multi-line buffer
    // Programlar yaşar: dahil edilen dosyaların AST'si derlenmiş koda bağlı kalabilir.
    std::vector<std::unique_ptr<Program>> owned_programs;

    while (true) {
        const char* prompt = pending.empty() ? ">>> " : "... ";
        char* raw = linenoise(prompt);

        if (!raw) {
            // Ctrl+C or Ctrl+D
            if (!pending.empty()) {
                // Cancel multi-line
                pending.clear();
                std::cout << "\n";
                continue;
            }
            std::cout << "\n" << gray("See you!") << "\n";
            break;
        }

        std::string line(raw);
        linenoise_free(raw);

        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();

        // Empty line
        if (line.empty()) {
            if (!pending.empty()) {
                // Enter on empty line in multi-line = try to execute
            } else {
                continue;
            }
        }

        // Colon commands only on fresh line (not inside multi-line)
        if (pending.empty() && is_colon_command(line)) {
            linenoiseHistoryAdd(line.c_str());
            bool cont = handle_colon_command(line, globals);
            if (!cont) {
                std::cout << gray("See you!") << "\n";
                break;
            }
            continue;
        }

        // Accumulate
        if (!pending.empty()) pending += "\n";
        pending += line;

        // Check if we have unclosed braces — if so, keep collecting
        int depth = open_braces(pending);
        if (depth > 0 && !line.empty()) continue;

        // Try to execute
        std::string source = pending;
        pending.clear();

        linenoiseHistoryAdd(source.c_str());

        // ── Parse ────────────────────────────────────────────────────────
        std::unique_ptr<Program> program;
        try {
            Lexer  lexer(source);
            Parser parser(lexer.scan_tokens());
            program = parser.parse();
        } catch (const LookParseError& e) {
            std::cout << red("Parse error: ") << e.what() << "\n";
            continue;
        } catch (const std::exception& e) {
            std::cout << red("Parse error: ") << e.what() << "\n";
            continue;
        }

        // Son cümle bir ifadeyse (atama değil) değeri gösterilir: `return <ifade>` olarak derlenir.
        // (Böylece REPL'de `strtoupper("a")` "sonuç kullanılmıyor" hatası vermez — sonuç gösteriliyor.)
        // Son cümle düz bir değişkene atamaysa (`$x = 5`) çalıştıktan sonra o değişken gösterilir.
        std::string shown_var;
        if (!program->statements.empty()) {
            if (auto* es = node_as<ExpressionStatement>(program->statements.back().get())) {
                if (auto* as = node_as<AssignmentExpression>(es->expression.get())) {
                    if (!as->object && !as->index) shown_var = as->name;
                } else {
                    auto ret = std::make_unique<ReturnStatement>(std::move(es->expression));
                    ret->loc = es->loc;
                    program->statements.back() = std::move(ret);
                }
            }
        }

        owned_programs.push_back(std::move(program));
        Program* prog_ptr = owned_programs.back().get();

        CompiledProgram compiled;
        try {
            compiled = Compiler::compile(*prog_ptr, std::filesystem::current_path().string());
        } catch (const std::exception& e) {
            std::cout << red("Error: ") << e.what() << "\n";
            continue;
        }
        // Satırın `use` ettiği modüller açılır (derleyicinin listesi); yerleşik tablo bu satırın
        // çıktısına bağlanır.
        for (const auto& m : compiled.use_modules) interp.load_stdlib_module(m);
        std::ostringstream captured;
        builtins = build_cli_builtins(interp, captured);
        wire_cli_callbacks(builtins, interp, callbacks);
        if (compiled.uses_non_builtin_module_fn) {
            for (const auto& n : compiled.non_builtin_module_fns)
                std::cout << red("Error: ") << n << "() does not exist — check the spelling of the module and the function.\n";
            continue;
        }

        // ── Execute ──────────────────────────────────────────────────────
        Value last_val;
        bool failed = false;
        {
            VM vm(sh, captured);
            vm.set_web_context(&web_ctx);
            vm.set_globals(globals);
            callbacks->main_vm = &vm;
            try {
                last_val = vm.execute(compiled);
            } catch (const RouteMatchedException&) {
                // fine
            } catch (const ExitException& e) {
                std::cout << captured.str() << gray("exit(" + std::to_string(e.code()) + ")") << "\n";
                failed = true;
            } catch (const std::exception& e) {
                std::cout << captured.str() << red("Error: ") << e.what() << "\n";
                failed = true;
            }
            globals = vm.get_globals();   // hata olsa da o ana kadar atananlar kalır
            callbacks->main_vm = nullptr;
        }
        if (failed) continue;

        // Print captured stdout (from print() calls)
        std::string out = captured.str();
        if (!out.empty()) std::cout << out;

        if (!shown_var.empty()) {
            auto it = globals.find(shown_var);
            if (it == globals.end() && shown_var[0] != '$') it = globals.find("$" + shown_var);
            if (it != globals.end()) last_val = it->second;
        }
        // Auto-display last expression value (if it's not null and nothing was printed)
        if (last_val.type() != Value::NONE && out.empty()) {
            std::cout << "=> " << repl_display(last_val) << "\n";
        }
    }

    linenoiseHistorySave(hist_file);
    return 0;
}

} // namespace look

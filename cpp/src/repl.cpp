// repl.cpp — `look repl` interactive session
#include "look/repl.h"
#include "look/interpreter.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/web.h"

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

// ── Open-brace counter — determines if input is complete ─────────────────
// Returns > 0 if there are unclosed { } blocks
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
        } else if (c == '{') depth++;
        else if (c == '}') depth--;
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
    case Value::FUNCTION:
    case Value::BYTECODE_FN:
        return gray("<function>");
    default:
        return gray(v.to_string());
    }
}

// ── :vars — list all globals defined by user ──────────────────────────────
static void print_vars(const Interpreter& interp) {
    auto vars = interp.get_global_names();
    if (vars.empty()) { std::cout << gray("  (henüz değişken yok)") << "\n"; return; }
    std::sort(vars.begin(), vars.end());
    for (auto& name : vars)
        std::cout << "  $" << name << "\n";
}

// ── :help ─────────────────────────────────────────────────────────────────
static void print_help() {
    std::cout << "\n" << cyan("LOOK REPL Komutları:") << "\n";
    std::cout << "  :help     — bu yardım\n";
    std::cout << "  :vars     — tanımlı değişkenler\n";
    std::cout << "  :clear    — ekranı temizle\n";
    std::cout << "  :exit     — çık (veya Ctrl+C / Ctrl+D)\n\n";
    std::cout << cyan("İpuçları:") << "\n";
    std::cout << "  Sonuç otomatik gösterilir — print() yazmana gerek yok\n";
    std::cout << "  Çok satırlı: { ile açılan blok } ile kapatılana kadar bekler\n";
    std::cout << "  Ok tuşları: komut geçmişi (↑/↓), imleç (←/→)\n\n";
}

// ── Wrap source as expression-or-statement ────────────────────────────────
// If the input looks like an expression (not an assignment, route, etc.),
// we wrap it in a special "eval print" form by compiling as expression.
// Strategy: try to parse as expression first; if it succeeds and produces
// a non-null value, display it. Otherwise interpret as statement.

static bool is_colon_command(const std::string& s) {
    return !s.empty() && s[0] == ':';
}

static bool handle_colon_command(const std::string& s, Interpreter& interp) {
    if (s == ":exit" || s == ":quit" || s == ":q") return false;  // signal exit
    if (s == ":help" || s == ":h") { print_help(); return true; }
    if (s == ":vars") { print_vars(interp); return true; }
    if (s == ":clear") { linenoiseClearScreen(); return true; }
    // :time <expr> — placeholder (future)
    if (s.substr(0,5) == ":time") {
        std::cout << yellow("  :time henüz implement edilmedi\n");
        return true;
    }
    std::cout << red("  Bilinmeyen komut: ") << s << "\n";
    std::cout << gray("  :help yazarak komutları görebilirsin\n");
    return true;
}

// ── Main REPL loop ────────────────────────────────────────────────────────
int run_repl() {
    enable_vt();
    linenoiseHistorySetMaxLen(200);

    // Try to load history from ~/.look_history
    const char* hist_file = ".look_history";
    linenoiseHistoryLoad(hist_file);

    std::cout << cyan("LOOK v0.20 REPL") << "\n";
    std::cout << gray("Çıkmak için :exit veya Ctrl+C") << "\n\n";

    // Shared persistent interpreter — state survives between lines
    Interpreter interp;
    WebContext  web_ctx;
    web_ctx.method = "GET";
    web_ctx.path   = "/";
    interp.set_web_context(&web_ctx);
    interp.set_file("<repl>");

    // Pre-load standard modules so user doesn't need `use X;` in REPL
    {
        const char* preamble =
            "use math; use string; use array; use type; use date; use file; "
            "use auth; use validator; use html; use http; use template;";
        std::ostringstream dev_null;
        interp.set_output(dev_null);
        try {
            Lexer  l(preamble);
            Parser p(l.scan_tokens());
            auto prog = p.parse();
            interp.interpret(*prog);
        } catch (...) {}
        interp.set_output(std::cout);
    }

    std::string pending;  // accumulated multi-line buffer
    // Keep all programs alive — LookFunction::body points into these ASTs
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
            std::cout << "\n" << gray("Görüşürüz!") << "\n";
            break;
        }

        std::string line(raw);
        linenoise_free(raw);

        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
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
            bool cont = handle_colon_command(line, interp);
            if (!cont) {
                std::cout << gray("Görüşürüz!") << "\n";
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
            std::cout << red("Parse hatası: ") << e.what() << "\n";
            continue;
        } catch (const std::exception& e) {
            std::cout << red("Parse hatası: ") << e.what() << "\n";
            continue;
        }

        // ── Execute ──────────────────────────────────────────────────────
        // We redirect output — if program prints, we show that.
        // Also capture "last value" from expression statements.
        std::ostringstream captured;
        interp.set_output(captured);

        Value last_val;
        bool has_last_val = false;

        // Keep program alive — LookFunction::body holds raw pointers into the AST
        owned_programs.push_back(std::move(program));
        Program* prog_ptr = owned_programs.back().get();

        try {
            // Set a callback so the interpreter records the last expression value
            interp.set_repl_value_callback([&](const Value& v) {
                last_val     = v;
                has_last_val = true;
            });

            interp.interpret(*prog_ptr);
        } catch (const RouteMatchedException&) {
            // fine
        } catch (const LookRuntimeError& e) {
            std::cout << red("Hata: ") << e.what() << "\n";
            interp.set_output(std::cout);
            continue;
        } catch (const ExitException& e) {
            std::cout << gray("exit(" + std::to_string(e.code()) + ")") << "\n";
            interp.set_output(std::cout);
            continue;
        } catch (const std::exception& e) {
            std::cout << red("Hata: ") << e.what() << "\n";
            interp.set_output(std::cout);
            continue;
        }

        interp.set_output(std::cout);

        // Print captured stdout (from print() calls)
        std::string out = captured.str();
        if (!out.empty()) std::cout << out;

        // Auto-display last expression value (if it's not null and nothing was printed)
        if (has_last_val && last_val.type() != Value::NONE && out.empty()) {
            std::cout << "=> " << repl_display(last_val) << "\n";
        }
    }

    linenoiseHistorySave(hist_file);
    return 0;
}

} // namespace look

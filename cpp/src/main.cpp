#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/test_runner.h"
#include "look/repl.h"
#include "look/web.h"
#include "look/installer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

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
    std::cout << "  look <source.lk>              — run a script\n";
    std::cout << "  look -c \"code\"                — run inline code\n";
    std::cout << "  look test                     — run all tests in tests/\n";
    std::cout << "  look test <pattern>           — run matching tests\n";
    std::cout << "  look test --verbose           — verbose output\n";
    std::cout << "  look repl                     — interactive REPL\n";
    std::cout << "  look install <pkg>            — install package (e.g. github.com/user/repo)\n";
    std::cout << "  look install <pkg@ref>        — install specific branch/tag\n";
    std::cout << "  look install                  — install all from look.lock\n";
    std::cout << "  look version                  — print version info\n";
}

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Unable to open file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) { print_usage(); return 1; }

        std::string cmd = argv[1];

        // ── look test [pattern] [--verbose] ──────────────────────────────────
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

        // ── look install [pkg] ────────────────────────────────────────────────
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

        // ── look <file> / look -c "code" ─────────────────────────────────────
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
            auto err = e;
            if (err.file.empty()) err.file = filename;
            std::cerr << err.format();
            return 1;
        } catch (const std::runtime_error& e) {
            std::cerr << "\nParse Error: " << e.what()
                      << "\n  File: " << filename << "\n";
            return 1;
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
            return 1;
        }

        return 0;
    } catch (const look::LookRuntimeError& e) {
        std::cerr << e.format() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

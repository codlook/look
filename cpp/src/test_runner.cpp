// test_runner.cpp — `look test` subcommand implementation
#include "look/read_stream.h"   // read_stream — dosyayı tek seferde oku
#include "look/test_runner.h"
#include "look/runtime_host.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/web.h"
#include "look/websocket.h"
#include "look/cli_runtime.h"   // build_cli_builtins / wire_cli_callbacks — lk ile aynı VM ortamı
#include "look/compiler.h"
#include "look/vm.h"
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <regex>
#include <set>
#include <string_view>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void set_console_color(int color) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color); }
static void reset_console_color()        { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7); }
#  define COLOR_GREEN   10
#  define COLOR_RED     12
#  define COLOR_YELLOW  14
#  define COLOR_CYAN    11
#  define COLOR_GRAY     8
#else
static void set_console_color(int code) { std::cout << "\033[" << code << "m"; }
static void reset_console_color()       { std::cout << "\033[0m"; }
#  define COLOR_GREEN   32
#  define COLOR_RED     31
#  define COLOR_YELLOW  33
#  define COLOR_CYAN    36
#  define COLOR_GRAY    90
#endif

namespace look {

namespace fs = std::filesystem;

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Could not open file: " + path.string());
    return look::read_stream(f);
}

// assert:: module — uses TestAssertionError to signal failure. `call` runs a LOOK function value
// (LOOK 2: on the VM that is running the test — vm_bridge_invoke).
using CallFn = std::function<void(const Value&)>;
static Module make_assert_module(CallFn call) {
    Module m;
    m.name = "assert";

    auto fail = [](const std::string& msg) {
        throw TestAssertionError(msg);
    };

    m.functions["ok"] = [fail](auto args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert::ok() failed — value is falsy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["eq"] = [fail](auto args) -> Value {
        if (args.size() < 2)
            fail("assert::eq() takes 2 arguments");
        auto& a = args[0]; auto& b = args[1];
        // Compare by string representation for simplicity
        bool eq = false;
        if (a.type() == b.type()) {
            if (a.type() == Value::INT)    eq = (a.to_int() == b.to_int());
            else if (a.type() == Value::FLOAT)  eq = (a.to_float() == b.to_float());
            else if (a.type() == Value::BOOL)   eq = (a.as_bool() == b.as_bool());
            else if (a.type() == Value::NONE)   eq = true;
            else eq = (a.to_string() == b.to_string());
        } else {
            eq = (a.to_string() == b.to_string());
        }
        if (!eq)
            fail("assert::eq() failed:\n    expected: " + b.to_string() +
                 "\n    actual:   " + a.to_string());
        return Value();
    };

    m.functions["neq"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::neq() takes 2 arguments");
        bool eq = (args[0].to_string() == args[1].to_string());
        if (eq)
            fail("assert::neq() failed — values are equal: " + args[0].to_string());
        return Value();
    };

    m.functions["null"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::NONE)
            fail("assert::null() failed — value is not null: " +
                 (args.empty() ? "(none)" : args[0].to_string()));
        return Value();
    };

    m.functions["not_null"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].type() == Value::NONE)
            fail("assert::not_null() failed — value is null");
        return Value();
    };

    m.functions["contains"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::contains() takes 2 arguments");
        auto& arr = args[0];
        auto& val = args[1];
        if (arr.type() != Value::ARRAY)
            fail("assert::contains() first argument must be an array");
        auto& vec = *arr.as_array();
        bool found = false;
        for (auto& elem : vec) {
            if (elem.to_string() == val.to_string()) { found = true; break; }
        }
        if (!found)
            fail("assert::contains() failed — \"" + val.to_string() + "\" is not in the array");
        return Value();
    };

    m.functions["true"] = [fail](auto args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert::true() failed — value is falsy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["false"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].is_truthy())
            fail("assert::false() failed — value is truthy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["match"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::match() takes 2 arguments: (str, regex)");
        std::string s     = args[0].to_string();
        std::string pat   = args[1].to_string();
        try {
            std::regex re(pat);
            if (!std::regex_search(s, re))
                fail("assert::match() failed — \"" + s + "\" did not match pattern: " + pat);
        } catch (std::regex_error& e) {
            fail("assert::match() invalid regex: " + std::string(e.what()));
        }
        return Value();
    };

    m.functions["throws"] = [fail, call](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::BYTECODE_FN)
            fail("assert::throws() expects a function");
        // Fonksiyonu HEMEN çağır ve hata fırlatmasını bekle. (Eskiden bir marker
        // array döndürüp runner'ın kontrol etmesini beklerdi — ama LOOK son ifadeyi
        // örtük DÖNDÜRMEDIĞI için marker atılıyordu; assert::throws() son ifade
        // olsa bile hiçbir zaman assert etmiyordu → sessiz yalancı geçiş.)
        bool threw = false;
        try { call(args[0]); } catch (...) { threw = true; }
        if (!threw)
            fail("assert::throws() failed — the call did not throw");
        return Value();
    };

    return m;
}

// ── ws:: test module ─────────────────────────────────────────────────────────
// Exposes ws::decode_frame(raw_bytes) for security regression tests.
// Only the frame-decode path is needed; full server machinery is not required.
static Module make_ws_test_module() {
    Module m;
    m.name = "ws";

    // ws::decode_frame(raw_string) → throws on oversized / malformed frames,
    // returns the payload string on success.
    m.functions["decode_frame"] = [](auto args) -> Value {
        if (args.empty())
            throw std::runtime_error("ws::decode_frame() expects 1 argument (raw frame bytes)");
        const std::string raw = args[0].to_string();
        WsFrame frame = ws_try_decode_frame(raw);
        // ws_try_decode_frame signals rejection by leaving frame.complete = false
        // OR by throwing when the internal resize would exceed limits (v1.3.1+).
        if (!frame.complete)
            throw std::runtime_error("ws::decode_frame: frame rejected (incomplete or oversized)");
        return Value(frame.payload);
    };

    return m;
}

// ── html:: test module ───────────────────────────────────────────────────────
// Exposes html::sanitize_svg(input) to .lk test files.
//
// Calls look::svg_sanitize() — the single source of truth defined in web.cpp.
// SVG_SAFE_ELEMENTS whitelist lives in web.h; both production and test use the
// same symbol.  When the whitelist changes, recompile — tests reflect the
// change automatically without any manual sync.
static Module make_html_test_module() {
    Module m;
    m.name = "html";

    m.functions["sanitize_svg"] = [](auto args) -> Value {
        if (args.empty())
            throw std::runtime_error("html::sanitize_svg() expects 1 argument");
        return Value(look::svg_sanitize(args[0].to_string()));
    };

    return m;
}

struct TestResult {
    std::string file;
    std::string name;
    bool        passed  = false;
    std::string message;         // failure message
    long long   elapsed_ms = 0;
};

static std::vector<fs::path> find_test_files(const std::string& pattern) {
    std::vector<fs::path> files;
    fs::path tests_dir = fs::current_path() / "tests";

    if (pattern.empty()) {
        // Look in tests/ directory
        if (!fs::exists(tests_dir)) {
            std::cerr << "tests/ folder not found: " << tests_dir.string() << "\n";
            return files;
        }
        for (auto& entry : fs::recursive_directory_iterator(tests_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lk")
                files.push_back(entry.path());
        }
    } else {
        // Pattern: "auth" → tests/test_auth.lk, or direct path
        fs::path direct(pattern);
        if (direct.extension() == ".lk" && fs::exists(direct)) {
            files.push_back(direct);
        } else if (direct.extension() == ".lk" && fs::exists(tests_dir / direct.filename())) {
            files.push_back(tests_dir / direct.filename());
        } else {
            // Try tests/test_<pattern>.lk and tests/<pattern>.lk
            fs::path p1 = tests_dir / ("test_" + pattern + ".lk");
            fs::path p2 = tests_dir / (pattern + ".lk");
            if (fs::exists(p1)) files.push_back(p1);
            else if (fs::exists(p2)) files.push_back(p2);
            else {
                // Glob in tests/ — any file containing pattern in name
                if (fs::exists(tests_dir)) {
                    for (auto& entry : fs::recursive_directory_iterator(tests_dir)) {
                        if (!entry.is_regular_file() || entry.path().extension() != ".lk") continue;
                        std::string stem = entry.path().stem().string();
                        if (stem.find(pattern) != std::string::npos)
                            files.push_back(entry.path());
                    }
                }
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

static std::vector<TestResult> run_file(const fs::path& file_path, bool verbose) {
    std::vector<TestResult> results;
    auto one = [&](const std::string& name, const std::string& msg) {
        TestResult r;
        r.file    = file_path.filename().string();
        r.name    = name;
        r.message = msg;
        results.push_back(r);
        return results;
    };
    std::string source;
    try { source = read_file(file_path); }
    catch (std::exception& e) { return one("(file read error)", e.what()); }

    // Parse
    look::Lexer  lexer(source);
    look::Parser parser(lexer.scan_tokens());
    std::unique_ptr<look::Program> program;
    try { program = parser.parse(); }
    catch (std::exception& e) { return one("(parse error)", e.what()); }

    // LOOK 2 (tek motor, adım 6): test dosyası VM'de koşar — komut satırıyla AYNI yerleşik tablo
    // (cli_runtime). Eskiden yorumlayıcıda koşuyordu; testler programın gerçekte koştuğu motoru
    // sınamıyordu. Test yardımcıları (test, assert_*, assert::*) yerleşik tabloda DEĞİL — `test`
    // adını bütün programlar için ayırırdı — global'e konan ev sahibi fonksiyonlarıdır.
    std::ostringstream out;   // test dosyasının çıktısı (eskisi gibi gösterilmez)
    look::WebContext web_ctx;
    web_ctx.method = "GET";
    web_ctx.path   = "/";

    look::RuntimeHost interp(out);   // modül deposu (ModuleHost) — LOOK kodu çalıştırmaz
    interp.set_web_context(&web_ctx);
    interp.set_file(file_path.string());
    const fs::path dir = fs::absolute(file_path).parent_path();
    // Modüller derlemeden sonra açılır (CompiledProgram::use_modules — aşağıda).

    // ── Ev sahibi fonksiyonları ─────────────────────────────────────────────
    struct Case { std::string name; Value fn; };
    std::vector<Case> cases;
    Value before_each_fn, after_each_fn;
    std::unordered_map<std::string, Value> host;
    auto host_fn = [&](const std::string& name, look::BuiltinFn fn) { host[name] = look::make_native_fn(name, std::move(fn)); };
    // Verilen LOOK fonksiyonunu o an testi koşturan VM'de çağırır.
    CallFn call = [](const Value& fn) { std::vector<Value> none; look::vm_bridge_invoke(fn, none); };

    host_fn("test", [&](std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::STRING)
            throw std::runtime_error("test() — expects test(name, function() {...})");
        if (args[1].type() != Value::BYTECODE_FN)
            throw std::runtime_error("test() — second argument must be a function");
        cases.push_back({args[0].as_string(), args[1]});
        return Value();
    });
    host_fn("before_each", [&](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type() != Value::BYTECODE_FN)
            throw std::runtime_error("before_each() — expects a function");
        before_each_fn = args[0];
        return Value();
    });
    host_fn("after_each", [&](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type() != Value::BYTECODE_FN)
            throw std::runtime_error("after_each() — expects a function");
        after_each_fn = args[0];
        return Value();
    });

    // `use assert` + assert::eq(...) — modül biçimi
    for (auto& [fname, fn] : make_assert_module(call).functions)
        host_fn("assert::" + fname, [fn = fn](std::vector<Value>& a) -> Value { return fn(a); });
    // Test kancaları: ws::decode_frame (WS çerçeve DoS testleri), html::sanitize_svg (SVG XSS testleri)
    for (auto& [fname, fn] : make_ws_test_module().functions)
        host_fn("ws::" + fname, [fn = fn](std::vector<Value>& a) -> Value { return fn(a); });
    for (auto& [fname, fn] : make_html_test_module().functions)
        host_fn("html::" + fname, [fn = fn](std::vector<Value>& a) -> Value { return fn(a); });

    // ── Top-level assert_*() shortcuts ──────────────────────────────────────
    auto fail = [](const std::string& msg) { throw TestAssertionError(msg); };

    host_fn("assert", [fail](std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert() failed — " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    host_fn("assert_true", [fail](std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert_true() failed — value is falsy: " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    host_fn("assert_false", [fail](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].is_truthy())
            fail("assert_false() failed — value is truthy: " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    host_fn("assert_eq", [fail](std::vector<Value>& args) -> Value {
        if (args.size() < 2) fail("assert_eq() takes 2 arguments");
        bool eq = false;
        auto& a = args[0]; auto& b = args[1];
        if (a.type() == b.type()) {
            if (a.type() == Value::INT)   eq = a.to_int()   == b.to_int();
            else if (a.type() == Value::FLOAT) eq = a.to_float() == b.to_float();
            else if (a.type() == Value::BOOL)  eq = a.as_bool()  == b.as_bool();
            else if (a.type() == Value::NONE)  eq = true;
            else eq = a.to_string() == b.to_string();
        } else eq = a.to_string() == b.to_string();
        if (!eq)
            fail("assert_eq() failed:\n    expected: " + b.to_string() + "\n    actual:   " + a.to_string());
        return Value();
    });

    host_fn("assert_neq", [fail](std::vector<Value>& args) -> Value {
        if (args.size() < 2) fail("assert_neq() takes 2 arguments");
        if (args[0].to_string() == args[1].to_string())
            fail("assert_neq() failed — values are equal: " + args[0].to_string());
        return Value();
    });

    host_fn("assert_null", [fail](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type() != Value::NONE)
            fail("assert_null() failed — value is not null: " + (args.empty() ? "(none)" : args[0].to_string()));
        return Value();
    });

    host_fn("assert_not_null", [fail](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type() == Value::NONE)
            fail("assert_not_null() failed — value is null");
        return Value();
    });

    host_fn("assert_contains", [fail](std::vector<Value>& args) -> Value {
        if (args.size() < 2) fail("assert_contains() takes 2 arguments");
        if (args[0].type() != Value::ARRAY)
            fail("assert_contains() first argument must be an array");
        auto& vec = *args[0].as_array();
        bool found = false;
        for (auto& elem : vec)
            if (elem.to_string() == args[1].to_string()) { found = true; break; }
        if (!found)
            fail("assert_contains() failed — \"" + args[1].to_string() + "\" is not in the array");
        return Value();
    });

    host_fn("assert_throws", [fail, call](std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type() != Value::BYTECODE_FN)
            fail("assert_throws() expects a function");
        // Fonksiyonu HEMEN çağır ve hata fırlatmasını bekle. Eski marker mekanizması yalnız testin
        // DÖNÜŞ değeriyken çalışıyordu; LOOK son ifadeyi örtük döndürmediği için belgelenen kullanım
        // dahil hiçbir zaman assert etmiyordu (sessiz yalancı geçiş).
        bool threw = false;
        try { call(args[0]); } catch (...) { threw = true; }
        if (!threw)
            fail("assert_throws() failed — the call did not throw");
        return Value();
    });

    host_fn("assert_match", [fail](std::vector<Value>& args) -> Value {
        if (args.size() < 2) fail("assert_match() takes 2 arguments: (str, regex)");
        std::string s = args[0].to_string(), pat = args[1].to_string();
        try {
            if (!std::regex_search(s, std::regex(pat)))
                fail("assert_match() failed — \"" + s + "\" did not match pattern: " + pat);
        } catch (std::regex_error& e) { fail("assert_match() invalid regex: " + std::string(e.what())); }
        return Value();
    });

    // ── Derle ───────────────────────────────────────────────────────────────
    look::CompiledProgram compiled;
    try { compiled = look::Compiler::compile(*program, dir.string(), {"assert", "ws"}); }
    catch (std::exception& e) { return one("(load error)", e.what()); }
    for (const auto& m : compiled.use_modules) interp.load_stdlib_module(m);   // derleyicinin listesi
    // Yerleşik de ev sahibi fonksiyonu da olmayan mod::fn → yazım hatası (komut satırındaki kural).
    for (const auto& n : compiled.non_builtin_module_fns)
        if (!host.count(n))
            return one("(load error)", n + "() does not exist — check the spelling of the module and the function");

    // ── Üst düzeyi bir kez çalıştır: test()'ler kaydolur ────────────────────
    static std::vector<look::BuiltinFn> builtins;   // geri çağırmalar dosyadan uzun yaşayabilir
    static look::VM::SharedState sh;
    builtins = look::build_cli_builtins(interp, out);
    sh.builtins = &builtins;
    auto callbacks = std::make_shared<look::CliCallbacks>();
    callbacks->shared = &sh;
    look::wire_cli_callbacks(builtins, interp, callbacks);
    std::unordered_map<std::string, Value> snapshot;
    {
        look::VM vm(sh, out);
        vm.set_web_context(&web_ctx);
        vm.set_globals(host);
        callbacks->main_vm = &vm;
        try {
            vm.execute(compiled);
        } catch (const look::RouteMatchedException&) {
            // OK
        } catch (std::exception& e) {
            callbacks->main_vm = nullptr;
            return one("(setup error)", e.what());
        }
        snapshot = vm.get_globals();
        callbacks->main_vm = nullptr;
    }

    // ── Her test: üst düzeyin durumundan başlayan kendi VM'i ────────────────
    auto run_in_fresh_vm = [&](const Value& fn, std::ostream& sink) {
        look::VM sub(sh, sink);
        sub.set_web_context(&web_ctx);
        sub.set_globals(snapshot);
        sub.call_closure(*fn.as_bytecode_fn(), {});
    };
    for (auto& tc : cases) {
        TestResult res;
        res.file = file_path.filename().string();
        res.name = tc.name;

        auto t0 = std::chrono::steady_clock::now();
        try {
            // Her test yalıtılmış: kendi VM'i, üst düzeyin bıraktığı global'ler (değer semantiği:
            // bir testin değiştirdiği dizi sonrakine geçmez).
            look::VM sub(sh, out);
            sub.set_web_context(&web_ctx);
            sub.set_globals(snapshot);
            // before_each — runs before the test body (e.g. db::begin)
            if (before_each_fn.type() == Value::BYTECODE_FN)
                sub.call_closure(*before_each_fn.as_bytecode_fn(), {});
            sub.call_closure(*tc.fn.as_bytecode_fn(), {});
            res.passed = true;
        } catch (const TestAssertionError& e) {
            res.passed  = false;
            res.message = e.what();
        } catch (std::exception& e) {
            res.passed  = false;
            res.message = "Runtime error: " + std::string(e.what());
        }

        // after_each — always runs (even on failure) — db::rollback pattern
        try {
            if (after_each_fn.type() == Value::BYTECODE_FN) {
                std::ostringstream dummy;
                run_in_fresh_vm(after_each_fn, dummy);
            }
        } catch (...) { /* after_each hataları sessizce yutulur */ }

        auto t1 = std::chrono::steady_clock::now();
        res.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        results.push_back(res);
    }

    if (results.empty()) {
        // File has no test() calls — warn
        if (verbose) {
            set_console_color(COLOR_YELLOW);
            std::cout << "  ⚠ no test() call\n";
            reset_console_color();
        }
    }

    return results;
}

int run_test_mode(const std::string& pattern, bool verbose) {
    auto files = find_test_files(pattern);

    if (files.empty()) {
        std::cerr << "Test file not found";
        if (!pattern.empty()) std::cerr << " (pattern: " << pattern << ")";
        std::cerr << "\n";
        return 1;
    }

    set_console_color(COLOR_CYAN);
    std::cout << "\nLOOK Test Runner v1.0\n";
    reset_console_color();
    std::cout << "\n";

    int total = 0, passed = 0, failed = 0;
    long long total_ms = 0;

    for (auto& file : files) {
        // Print file header
        set_console_color(COLOR_GRAY);
        std::cout << file.filename().string() << "\n";
        reset_console_color();

        auto results = run_file(file, verbose);

        for (auto& r : results) {
            total++;
            total_ms += r.elapsed_ms;

            if (r.passed) {
                passed++;
                set_console_color(COLOR_GREEN);
                std::cout << "  ✅ " << r.name;
                reset_console_color();
                set_console_color(COLOR_GRAY);
                std::cout << " (" << r.elapsed_ms << "ms)";
                reset_console_color();
                std::cout << "\n";
            } else {
                failed++;
                set_console_color(COLOR_RED);
                std::cout << "  ❌ " << r.name;
                reset_console_color();
                std::cout << "\n";
                if (!r.message.empty()) {
                    set_console_color(COLOR_YELLOW);
                    // Indent the failure message
                    std::istringstream ss(r.message);
                    std::string line;
                    while (std::getline(ss, line))
                        std::cout << "     " << line << "\n";
                    reset_console_color();
                }
            }
        }
        std::cout << "\n";
    }

    // Summary line
    if (failed == 0) {
        set_console_color(COLOR_GREEN);
        std::cout << passed << "/" << total << " passed";
        reset_console_color();
    } else {
        set_console_color(COLOR_RED);
        std::cout << failed << " failed";
        reset_console_color();
        std::cout << " — ";
        set_console_color(COLOR_GREEN);
        std::cout << passed << "/" << total << " passed";
        reset_console_color();
    }
    std::cout << " — " << total_ms << "ms\n\n";

    return (failed > 0) ? 1 : 0;
}

} // namespace look

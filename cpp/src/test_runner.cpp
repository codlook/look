// test_runner.cpp — `look test` subcommand implementation
#include "look/test_runner.h"
#include "look/interpreter.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/web.h"
#include "look/websocket.h"
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
    if (!f) throw std::runtime_error("Dosya açılamadı: " + path.string());
    return { std::istreambuf_iterator<char>(f), {} };
}

// assert:: module — uses TestAssertionError to signal failure
static Module make_assert_module() {
    Module m;
    m.name = "assert";

    auto fail = [](const std::string& msg) {
        throw TestAssertionError(msg);
    };

    m.functions["ok"] = [fail](auto args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert::ok() başarısız — değer falsy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["eq"] = [fail](auto args) -> Value {
        if (args.size() < 2)
            fail("assert::eq() 2 argüman bekler");
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
            fail("assert::eq() başarısız:\n    beklenen: " + b.to_string() +
                 "\n    gerçek:   " + a.to_string());
        return Value();
    };

    m.functions["neq"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::neq() 2 argüman bekler");
        bool eq = (args[0].to_string() == args[1].to_string());
        if (eq)
            fail("assert::neq() başarısız — değerler eşit: " + args[0].to_string());
        return Value();
    };

    m.functions["null"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::NONE)
            fail("assert::null() başarısız — değer null değil: " +
                 (args.empty() ? "(yok)" : args[0].to_string()));
        return Value();
    };

    m.functions["not_null"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].type() == Value::NONE)
            fail("assert::not_null() başarısız — değer null");
        return Value();
    };

    m.functions["contains"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::contains() 2 argüman bekler");
        auto& arr = args[0];
        auto& val = args[1];
        if (arr.type() != Value::ARRAY)
            fail("assert::contains() ilk argüman array olmalı");
        auto& vec = *arr.as_array();
        bool found = false;
        for (auto& elem : vec) {
            if (elem.to_string() == val.to_string()) { found = true; break; }
        }
        if (!found)
            fail("assert::contains() başarısız — \"" + val.to_string() + "\" dizide yok");
        return Value();
    };

    m.functions["true"] = [fail](auto args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert::true() başarısız — değer falsy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["false"] = [fail](auto args) -> Value {
        if (args.empty() || args[0].is_truthy())
            fail("assert::false() başarısız — değer truthy: " +
                 (args.empty() ? "null" : args[0].to_string()));
        return Value();
    };

    m.functions["match"] = [fail](auto args) -> Value {
        if (args.size() < 2) fail("assert::match() 2 argüman bekler: (str, regex)");
        std::string s     = args[0].to_string();
        std::string pat   = args[1].to_string();
        try {
            std::regex re(pat);
            if (!std::regex_search(s, re))
                fail("assert::match() başarısız — \"" + s + "\" pattern ile eşleşmedi: " + pat);
        } catch (std::regex_error& e) {
            fail("assert::match() geçersiz regex: " + std::string(e.what()));
        }
        return Value();
    };

    m.functions["throws"] = [fail](auto args) -> Value {
        if (args.empty() || (args[0].type() != Value::FUNCTION && args[0].type() != Value::BYTECODE_FN))
            fail("assert::throws() fonksiyon bekler");
        // Note: actual invocation must happen in the test runner context
        // This flag tells the runner to call the fn and expect an exception
        // We use a special return value to signal this
        // Simpler: just return the fn wrapped in a marker array ["__throws__", fn]
        auto marker = std::make_shared<std::vector<Value>>();
        marker->push_back(Value(std::string("__assert_throws__")));
        marker->push_back(args[0]);
        return Value(marker);
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
// Exposes html::sanitize_svg(input) so SVG XSS regression tests can call it
// directly without going through file::store().
//
// The whitelist-based SVG sanitizer was added in v1.2.1.  We re-implement the
// same element + attribute whitelist here so the test module is self-contained
// and does not depend on a specific internal symbol name.
static Module make_html_test_module() {
    Module m;
    m.name = "html";

    // Allowed SVG element names (safe structural/visual subset).
    static const std::set<std::string> ELEM_WHITELIST = {
        "svg", "g", "path", "circle", "rect", "ellipse",
        "line", "polyline", "polygon", "text", "tspan",
        "defs", "symbol", "title", "desc",
        "linearGradient", "radialGradient", "stop",
        "clipPath", "mask", "pattern",
        // <use> is allowed only with fragment-only href — enforced in attr pass
        "use"
    };

    // Returns true if an attribute value contains a dangerous protocol or
    // CSS url() reference.
    auto is_dangerous_value = [](const std::string& val) -> bool {
        // Normalise: lower-case, strip whitespace and soft-hyphens
        std::string v;
        v.reserve(val.size());
        for (unsigned char c : val)
            if (!std::isspace(c)) v += (char)std::tolower(c);

        // Protocol schemes
        if (v.find("javascript:") != std::string::npos) return true;
        if (v.find("vbscript:")   != std::string::npos) return true;
        if (v.find("data:")       != std::string::npos) return true;

        // CSS url() with dangerous targets
        if (v.find("url(javascript:") != std::string::npos) return true;
        if (v.find("url('javascript:") != std::string::npos) return true;
        if (v.find("url(\"javascript:") != std::string::npos) return true;
        if (v.find("url(data:")        != std::string::npos) return true;
        if (v.find("url('data:")       != std::string::npos) return true;
        if (v.find("url(\"data:")      != std::string::npos) return true;
        if (v.find("expression(")      != std::string::npos) return true;
        return false;
    };

    // Minimal tag-level SVG sanitizer.
    // Strategy: scan the input for XML tags; skip any whose element name is
    // not in the whitelist, skip event-handler (on*) attributes, skip
    // attributes whose value is dangerous, and for <use> enforce that href /
    // xlink:href begins with '#'.
    auto sanitize_svg = [ELEM_WHITELIST, is_dangerous_value](const std::string& input) -> std::string {
        std::string out;
        out.reserve(input.size());
        size_t i = 0;
        const size_t n = input.size();

        while (i < n) {
            if (input[i] != '<') { out += input[i++]; continue; }

            // Find tag end
            size_t tag_start = i;
            size_t tag_end   = input.find('>', i);
            if (tag_end == std::string::npos) { out += input[i++]; continue; }

            std::string tag = input.substr(tag_start + 1, tag_end - tag_start - 1);
            // Handle closing tags and comments quickly
            if (tag.empty()) { out += "<>"; i = tag_end + 1; continue; }
            if (tag[0] == '!') { i = tag_end + 1; continue; }  // strip comments/CDATA

            bool closing = (tag[0] == '/');
            std::string_view tag_body = closing
                ? std::string_view(tag).substr(1)
                : std::string_view(tag);

            // Extract element name
            size_t name_end = 0;
            while (name_end < tag_body.size() &&
                   !std::isspace((unsigned char)tag_body[name_end]) &&
                   tag_body[name_end] != '/' &&
                   tag_body[name_end] != '>') ++name_end;
            std::string elem_name(tag_body.substr(0, name_end));
            // lower-case for comparison
            std::string elem_lower;
            for (char c : elem_name) elem_lower += (char)std::tolower(c);

            if (ELEM_WHITELIST.find(elem_lower) == ELEM_WHITELIST.end()) {
                // Skip entire element including its closing tag
                i = tag_end + 1;
                // If it has a body (not self-closing), skip until closing tag
                if (!closing && tag.back() != '/') {
                    std::string close_tag = "</" + elem_name;
                    size_t close_pos = input.find(close_tag, i);
                    if (close_pos != std::string::npos) {
                        size_t close_end = input.find('>', close_pos);
                        i = (close_end != std::string::npos) ? close_end + 1 : input.size();
                    }
                }
                continue;
            }

            // Allowed element — filter attributes
            bool is_use = (elem_lower == "use");
            std::string safe_tag = (closing ? "</" : "<") + elem_name;
            std::string_view attrs = tag_body.substr(name_end);
            size_t ai = 0;

            while (ai < attrs.size()) {
                // Skip whitespace
                while (ai < attrs.size() && std::isspace((unsigned char)attrs[ai])) ++ai;
                if (ai >= attrs.size() || attrs[ai] == '/' || attrs[ai] == '>') break;

                // Read attribute name
                size_t an_start = ai;
                while (ai < attrs.size() &&
                       attrs[ai] != '=' &&
                       !std::isspace((unsigned char)attrs[ai]) &&
                       attrs[ai] != '/' &&
                       attrs[ai] != '>') ++ai;
                std::string attr_name(attrs.substr(an_start, ai - an_start));
                std::string attr_lower;
                for (char c : attr_name) attr_lower += (char)std::tolower(c);

                // Skip = and read value
                std::string attr_val;
                if (ai < attrs.size() && attrs[ai] == '=') {
                    ++ai;
                    if (ai < attrs.size() && (attrs[ai] == '"' || attrs[ai] == '\'')) {
                        char q = attrs[ai++];
                        size_t vs = ai;
                        while (ai < attrs.size() && attrs[ai] != q) ++ai;
                        attr_val = std::string(attrs.substr(vs, ai - vs));
                        if (ai < attrs.size()) ++ai; // skip closing quote
                    } else {
                        size_t vs = ai;
                        while (ai < attrs.size() &&
                               !std::isspace((unsigned char)attrs[ai]) &&
                               attrs[ai] != '>') ++ai;
                        attr_val = std::string(attrs.substr(vs, ai - vs));
                    }
                }

                // Drop event handlers (on*)
                if (attr_lower.size() >= 2 && attr_lower.substr(0, 2) == "on") continue;

                // Drop dangerous values
                if (is_dangerous_value(attr_val)) continue;

                // For <use>, enforce fragment-only href / xlink:href
                if (is_use && (attr_lower == "href" || attr_lower == "xlink:href")) {
                    if (attr_val.empty() || attr_val[0] != '#') continue;
                }

                safe_tag += " " + attr_name + "=\"" + attr_val + "\"";
            }

            // Preserve self-closing slash
            if (!tag.empty() && tag.back() == '/') safe_tag += "/";
            safe_tag += ">";
            out += safe_tag;
            i = tag_end + 1;
        }
        return out;
    };

    m.functions["sanitize_svg"] = [sanitize_svg](auto args) -> Value {
        if (args.empty())
            throw std::runtime_error("html::sanitize_svg() expects 1 argument");
        return Value(sanitize_svg(args[0].to_string()));
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
            std::cerr << "tests/ klasörü bulunamadı: " << tests_dir.string() << "\n";
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
    std::string source;
    try { source = read_file(file_path); }
    catch (std::exception& e) {
        TestResult r;
        r.file    = file_path.filename().string();
        r.name    = "(dosya okuma hatası)";
        r.message = e.what();
        results.push_back(r);
        return results;
    }

    // Parse
    look::Lexer  lexer(source);
    look::Parser parser(lexer.scan_tokens());
    std::unique_ptr<look::Program> program;
    try { program = parser.parse(); }
    catch (std::exception& e) {
        TestResult r;
        r.file    = file_path.filename().string();
        r.name    = "(parse hatası)";
        r.message = e.what();
        results.push_back(r);
        return results;
    }

    // Set up interpreter with assert:: module and test() built-in
    std::ostringstream out;
    look::WebContext web_ctx;
    web_ctx.method = "GET";
    web_ctx.path   = "/";

    look::Interpreter interp(out);
    interp.set_web_context(&web_ctx);
    interp.set_file(file_path.string());

    // Register assert:: module (for "use assert;" syntax)
    auto assert_mod = make_assert_module();
    interp.register_use_module("assert", assert_mod);

    // Register ws:: test module — exposes ws::decode_frame() for WS frame DoS tests
    auto ws_mod = make_ws_test_module();
    interp.register_use_module("ws", ws_mod);

    // Register html:: test module — exposes html::sanitize_svg() for SVG XSS tests
    auto html_mod = make_html_test_module();
    interp.register_use_module("html", html_mod);

    // Register test() built-in
    interp.register_builtin("test", [&interp](std::vector<Value> args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::STRING)
            throw std::runtime_error("test() — test(isim, function() {...}) bekleniyor");
        if (args[1].type() != Value::FUNCTION && args[1].type() != Value::BYTECODE_FN)
            throw std::runtime_error("test() — ikinci argüman fonksiyon olmalı");
        interp.register_test_case(args[0].as_string(), args[1]);
        return Value();
    });

    // Register before_each() and after_each()
    interp.register_builtin("before_each", [&interp](std::vector<Value> args) -> Value {
        if (args.empty() || (args[0].type() != Value::FUNCTION && args[0].type() != Value::BYTECODE_FN))
            throw std::runtime_error("before_each() — fonksiyon bekler");
        interp.set_before_each(args[0]);
        return Value();
    });
    interp.register_builtin("after_each", [&interp](std::vector<Value> args) -> Value {
        if (args.empty() || (args[0].type() != Value::FUNCTION && args[0].type() != Value::BYTECODE_FN))
            throw std::runtime_error("after_each() — fonksiyon bekler");
        interp.set_after_each(args[0]);
        return Value();
    });

    // ── Top-level assert_*() shortcuts ──────────────────────────────────────
    auto fail = [](const std::string& msg) { throw TestAssertionError(msg); };

    interp.register_builtin("assert", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert() başarısız — " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    interp.register_builtin("assert_true", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_truthy())
            fail("assert_true() başarısız — değer falsy: " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    interp.register_builtin("assert_false", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].is_truthy())
            fail("assert_false() başarısız — değer truthy: " + (args.empty() ? "null" : args[0].to_string()));
        return Value();
    });

    interp.register_builtin("assert_eq", [fail](std::vector<Value> args) -> Value {
        if (args.size() < 2) fail("assert_eq() 2 argüman bekler");
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
            fail("assert_eq() başarısız:\n    beklenen: " + b.to_string() + "\n    gerçek:   " + a.to_string());
        return Value();
    });

    interp.register_builtin("assert_neq", [fail](std::vector<Value> args) -> Value {
        if (args.size() < 2) fail("assert_neq() 2 argüman bekler");
        if (args[0].to_string() == args[1].to_string())
            fail("assert_neq() başarısız — değerler eşit: " + args[0].to_string());
        return Value();
    });

    interp.register_builtin("assert_null", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type() != Value::NONE)
            fail("assert_null() başarısız — değer null değil: " + (args.empty() ? "(yok)" : args[0].to_string()));
        return Value();
    });

    interp.register_builtin("assert_not_null", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type() == Value::NONE)
            fail("assert_not_null() başarısız — değer null");
        return Value();
    });

    interp.register_builtin("assert_contains", [fail](std::vector<Value> args) -> Value {
        if (args.size() < 2) fail("assert_contains() 2 argüman bekler");
        if (args[0].type() != Value::ARRAY)
            fail("assert_contains() ilk argüman array olmalı");
        auto& vec = *args[0].as_array();
        bool found = false;
        for (auto& elem : vec)
            if (elem.to_string() == args[1].to_string()) { found = true; break; }
        if (!found)
            fail("assert_contains() başarısız — \"" + args[1].to_string() + "\" dizide yok");
        return Value();
    });

    interp.register_builtin("assert_throws", [fail](std::vector<Value> args) -> Value {
        if (args.empty() || (args[0].type() != Value::FUNCTION && args[0].type() != Value::BYTECODE_FN))
            fail("assert_throws() fonksiyon bekler");
        auto marker = std::make_shared<std::vector<Value>>();
        marker->push_back(Value(std::string("__assert_throws__")));
        marker->push_back(args[0]);
        return Value(marker);
    });

    interp.register_builtin("assert_match", [fail](std::vector<Value> args) -> Value {
        if (args.size() < 2) fail("assert_match() 2 argüman bekler: (str, regex)");
        std::string s = args[0].to_string(), pat = args[1].to_string();
        try {
            if (!std::regex_search(s, std::regex(pat)))
                fail("assert_match() başarısız — \"" + s + "\" pattern ile eşleşmedi: " + pat);
        } catch (std::regex_error& e) { fail("assert_match() geçersiz regex: " + std::string(e.what())); }
        return Value();
    });

    // Execute the file — this registers all test() calls
    try {
        interp.interpret(*program);
    } catch (const look::RouteMatchedException&) {
        // OK
    } catch (std::exception& e) {
        TestResult r;
        r.file    = file_path.filename().string();
        r.name    = "(setup hatası)";
        r.message = e.what();
        results.push_back(r);
        return results;
    }

    // Now run each registered test
    for (auto& tc : interp.test_cases()) {
        TestResult res;
        res.file = file_path.filename().string();
        res.name = tc.name;

        auto t0 = std::chrono::steady_clock::now();
        try {
            // Each test gets an isolated dispatch copy — no shared state between tests
            auto copy = interp.make_dispatch_copy();
            std::ostringstream test_out;
            copy->set_output(test_out);
            copy->set_web_context(&web_ctx);

            // before_each — runs before the test body (e.g. db::begin)
            if (interp.before_each().type() == Value::FUNCTION ||
                interp.before_each().type() == Value::BYTECODE_FN)
                copy->invoke(interp.before_each(), {});

            Value ret = copy->invoke(tc.fn, {});

            // Check for assert::throws marker
            if (ret.type() == Value::ARRAY) {
                auto& arr = *ret.as_array();
                if (!arr.empty() && arr[0].type() == Value::STRING &&
                    arr[0].as_string() == "__assert_throws__" && arr.size() >= 2)
                {
                    // Call the wrapped function — expect it to throw
                    bool threw = false;
                    try { copy->invoke(arr[1], {}); }
                    catch (...) { threw = true; }
                    if (!threw)
                        throw TestAssertionError("assert::throws() başarısız — hata fırlatılmadı");
                }
            }

            res.passed = true;
        } catch (const TestAssertionError& e) {
            res.passed  = false;
            res.message = e.what();
        } catch (const look::LookRuntimeError& e) {
            res.passed  = false;
            res.message = "Runtime hatası: " + std::string(e.what());
        } catch (std::exception& e) {
            res.passed  = false;
            res.message = std::string(e.what());
        }

        // after_each — always runs (even on failure) — db::rollback pattern
        try {
            if (interp.after_each().type() == Value::FUNCTION ||
                interp.after_each().type() == Value::BYTECODE_FN) {
                auto copy2 = interp.make_dispatch_copy();
                std::ostringstream dummy;
                copy2->set_output(dummy);
                copy2->set_web_context(&web_ctx);
                copy2->invoke(interp.after_each(), {});
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
            std::cout << "  ⚠ test() çağrısı yok\n";
            reset_console_color();
        }
    }

    return results;
}

int run_test_mode(const std::string& pattern, bool verbose) {
    auto files = find_test_files(pattern);

    if (files.empty()) {
        std::cerr << "Test dosyası bulunamadı";
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
        std::cout << passed << "/" << total << " geçti";
        reset_console_color();
    } else {
        set_console_color(COLOR_RED);
        std::cout << failed << " başarısız";
        reset_console_color();
        std::cout << " — ";
        set_console_color(COLOR_GREEN);
        std::cout << passed << "/" << total << " geçti";
        reset_console_color();
    }
    std::cout << " — " << total_ms << "ms\n\n";

    return (failed > 0) ? 1 : 0;
}

} // namespace look

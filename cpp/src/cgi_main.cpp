#include "look/mem_budget.h"   // memory budget (LOOK_MEMORY_LIMIT)
#include "look/lexer.h"
#include "look/parser.h"
#include "look/runtime_host.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/runtime_init.h"   // runtime_init (CA probe + SQLite init, process start — shared by the 3 entry points)
#include "look/session_files.h"  // session cleanup after the response (short-lived process)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <cstring>
#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#endif

namespace fs = std::filesystem;

// Shared web engine — http_main.cpp (same as lk-fcgi)
bool look_http_engine_ensure(const std::string& script_path, std::string& err);
void look_app_dispatch(look::WebContext& web, std::ostringstream& output, const std::string& prof_path);
void look_http_engine_set_quiet(bool quiet);   // no startup info lines — every request is a separate process

// ── Error response ────────────────────────────────────────────────────────────

static bool is_production() {
    const char* env = std::getenv("APP_ENV");
    const char* dbg = std::getenv("APP_DEBUG");
    bool debug = dbg && std::string(dbg) == "true";
    bool dev   = env && std::string(env) == "development";
    return !dev && !debug;
}

// development: detailed error | production: clean page
static void send_error(int status, const std::string& title, const std::string& msg) {
    // Always JSON — this is an API runtime
    std::cout << "Status: " << status << " " << title << "\r\n"
              << "Content-Type: application/json; charset=utf-8\r\n"
              << "Access-Control-Allow-Origin: *\r\n\r\n";

    if (is_production()) {
        // Production: generic message to the user, details go only to the log
        // "error"/"code" = the keys. "hata"/"kod" are deprecated copies: removed in the release that turns
        // the push()/pop() warnings into errors (announced in the docs, Error system section).
        std::cout << "{\"ok\":false,\"error\":\"Server error. Please try again.\",\"code\":" << status
                  << ",\"hata\":\"Server error. Please try again.\","
                  << "\"kod\":" << status << "}\n";
    } else {
        // Development: error details inside the JSON
        // escape special characters in msg
        std::string safe;
        for (char c : msg) {
            if      (c == '"')  safe += "\\\"";
            else if (c == '\\') safe += "\\\\";
            else if (c == '\n') safe += "\\n";
            else if (c == '\r') safe += "\\r";
            else                safe += c;
        }
        std::cout << "{\"ok\":false,\"error\":\"" << safe << "\",\"code\":" << status
                  << ",\"hata\":\"" << safe << "\","
                  << "\"kod\":" << status << "}\n";
    }
}

// ── Resolve script path ───────────────────────────────────────────────────────

#include "look/path_guard.h"   // canonical symlink/traversal gate (a string check is not enough)

static std::string resolve_script(int argc, char* argv[]) {
    // Path traversal rejection — the resolved .lk derives from client-influenced CGI envs and
    // is executed by LOOK as CODE; ".." escaping the doc root would allow arbitrary
    // .lk execution (RCE). (Same guard as resolve_script in fcgi_main.cpp.)
    auto no_traversal = [](const std::string& s) {
        return s.find("../") == std::string::npos &&
               s.find("..\\") == std::string::npos &&
               !(s.size() >= 2 && s.substr(s.size() - 2) == "..");
    };
    auto ends_lk = [&](const char* s) -> bool {
        if (!s || !*s) return false;
        std::string str(s);
        if (!no_traversal(str)) return false;
        return str.size() > 3 && str.substr(str.size() - 3) == ".lk";
    };

    const char* path_translated = std::getenv("PATH_TRANSLATED");
    const char* script_filename = std::getenv("SCRIPT_FILENAME");
    const char* path_info       = std::getenv("PATH_INFO");
    const char* document_root   = std::getenv("DOCUMENT_ROOT");

    if (ends_lk(path_translated)) return path_translated;
    if (ends_lk(script_filename)) return script_filename;

    // Apache Action directive: PATH_INFO = /index.lk/menu/burger-cafe
    // Extract just the .lk script part: DOCUMENT_ROOT + /index.lk
    if (path_info && *path_info && document_root && *document_root && no_traversal(path_info)) {
        std::string pi(path_info);
        size_t lk_pos = pi.find(".lk");
        // CANONICAL GATE: the path is built by LOOK from DOCUMENT_ROOT+PATH_INFO; the string no_traversal
        // cannot see a symlink → weakly_canonical rejects a candidate that leaves the doc root (symlink/..).
        std::string cand = (lk_pos != std::string::npos)
            ? std::string(document_root) + pi.substr(0, lk_pos + 3)
            : std::string(document_root) + pi;
        if (look::path_under_root(document_root, cand)) return cand;
        return "";
    }

    if (argc >= 2) return argv[1];
    return "";
}

// ── main ──────────────────────────────────────────────────────────────────────

#ifndef LOOK_VERSION
#define LOOK_VERSION "1.0.0"   // fallback; normally CMake compile-def provides it (single source)
#endif
#ifndef LOOK_BUILD
#define LOOK_BUILD "src"
#endif
#ifdef _WIN32
#  define LOOK_CGI_PLATFORM "windows"
#elif defined(__linux__)
#  define LOOK_CGI_PLATFORM "linux"
#elif defined(__APPLE__)
#  define LOOK_CGI_PLATFORM "darwin"
#else
#  define LOOK_CGI_PLATFORM "unknown"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#  define LOOK_CGI_ARCH "amd64"
#elif defined(__aarch64__)
#  define LOOK_CGI_ARCH "arm64"
#else
#  define LOOK_CGI_ARCH "x86"
#endif

int main(int argc, char* argv[]) {
    // lk-cgi --version (same format as lk / lk-fcgi): only OUTSIDE the CGI environment. It did not exist before —
    // it was not possible to see which lk-cgi was installed, the build stamp did not enter the binary (the release
    // consistency check could not verify it for that reason). In CGI the web server may pass a query string
    // without "=" as an argument: "?--version" must not reveal the version to the outside.
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0 && !std::getenv("GATEWAY_INTERFACE")) {
        std::cout << "LOOK " LOOK_VERSION " (" << LOOK_BUILD << ", " << LOOK_CGI_PLATFORM << "/" << LOOK_CGI_ARCH << ")\n";
        return 0;
    }
#ifdef _WIN32
    // CGI output must be byte-exact: text-mode \n→\r\n conversion breaks the CGI header/body
    // structure (the code already has \r\n → it becomes \r\r\n, the body is lost).
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Static OpenSSL: the embedded CA path is pinned to the build distribution (AlmaLinux); on another
    // distribution (Ubuntu) that path does not exist → default_verify_paths finds no CA and even a VALID certificate
    // is rejected. main.cpp/fcgi_main.cpp did this at startup; cgi_main SKIPPED it
    // → in CGI mode http:: + PostgreSQL TLS (verify DEFAULT) was broken across distributions.
    // Sets SSL_CERT_FILE process-globally; the DB clients' default_verify_paths reads it.
    look::runtime_init();  // all process-start init (CA probe + SQLite one-time init) — shared by the 3 entry points
    // Memory budget (look/mem_budget.h) — same rule as lk-fcgi; since every request is a separate process
    // it is not written to the log, only an invalid LOOK_MEMORY_LIMIT is reported.
    {
        std::string why, describe;
        if (!look::mem_budget_configure(why, describe)) {
            std::cerr << "Error: LOOK could not start: " << why << "\n";
            send_error(500, "Internal Server Error", "LOOK could not start; see the server log.");
            return 1;
        }
    }
    // In CGI the process lives for a single request: cleanup of expired session files (when due)
    // is done AFTER the response is sent. Output is closed first: the web server learns the response is finished
    // from the output closing, so the visitor does not wait for the sweep. Covers every return path.
    look::session_gc_short_lived(true);
    struct SessionGcAfterResponse {
        ~SessionGcAfterResponse() {
            std::cout.flush();
            std::fflush(stdout);
#ifndef _WIN32
            int nul = ::open("/dev/null", O_WRONLY);
            if (nul >= 0) { ::dup2(nul, 1); ::close(nul); }
#endif
            look::session_gc_run_pending();
        }
    } session_gc_after_response;
    std::string script_path = resolve_script(argc, argv);

    if (script_path.empty()) {
        send_error(400, "Bad Request", "No LOOK script specified.");
        return 1;
    }

    try {
        fs::path script(script_path);

        // ── .env path fix ─────────────────────────────────────────────────────
        // In CGI the working directory may be Apache's own folder.
        // Look for the .env file in the folder where the script is located.
        fs::path script_dir = script.parent_path();
        if (!script_dir.empty()) {
            // Go to the script directory — env() and log:: will work from here
#ifdef _WIN32
            _wchdir(script_dir.wstring().c_str());
#else
            (void)chdir(script_dir.string().c_str());
#endif
        }

        // Single engine, step 7: CGI also uses lk-fcgi's web engine — setup (once in the top-level
        // VM), route matching, 404, script mode, error response. CGI used to run the script from scratch in the interpreter on
        // every request: the same program ran with two separate engines in CGI and in FastCGI. A CGI process
        // lives for a single request: "setup + one request" =
        // the old "run the script once".
        std::string setup_err;
        look_http_engine_set_quiet(true);   // stderr = Apache error log: silent on a successful request
        if (!look_http_engine_ensure(script.string(), setup_err)) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI",
                "Setup error: " + setup_err + " | " + script_path);
            send_error(500, "Setup Error", setup_err);
            return 1;
        }

        look::WebContext web;
        web.init_from_cgi();
        std::ostringstream output;
        look_app_dispatch(web, output, web.path);

        // ── Response ──────────────────────────────────────────────────────────
        // Same as FastCGI: the response:: body if there is one, otherwise the print output.
        std::cout << web.build_headers();
        std::cout << (web.response_body.empty() ? output.str() : web.response_body);

    } catch (const std::exception& e) {
        // Production: write to the log, clean page to the user
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI",
            std::string("Uncaught exception: ") + e.what() +
            " | Script: " + script_path);

        send_error(500, "Internal Server Error", e.what());
        return 1;
    }

    return 0;
}

#include "look/lexer.h"
#include "look/parser.h"
#include "look/interpreter.h"
#include "look/logger.h"
#include "look/web.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <cstring>

namespace fs = std::filesystem;

// ── HTML escape ───────────────────────────────────────────────────────────────

static std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

// ── Error response ────────────────────────────────────────────────────────────

static bool is_production() {
    const char* env = std::getenv("APP_ENV");
    const char* dbg = std::getenv("APP_DEBUG");
    bool debug = dbg && std::string(dbg) == "true";
    bool dev   = env && std::string(env) == "development";
    return !dev && !debug;
}

// development: detayli hata | production: temiz sayfa
static void send_error(int status, const std::string& title, const std::string& msg) {
    // Her zaman JSON — bu bir API runtime'i
    std::cout << "Status: " << status << " " << title << "\r\n"
              << "Content-Type: application/json; charset=utf-8\r\n"
              << "Access-Control-Allow-Origin: *\r\n\r\n";

    if (is_production()) {
        // Production: kullaniciya genel mesaj, detay sadece log'a gider
        std::cout << "{\"ok\":false,\"hata\":\"Sunucu hatasi. Lutfen tekrar deneyin.\","
                  << "\"kod\":" << status << "}\n";
    } else {
        // Development: hata detayi JSON icinde
        // msg icindeki ozel karakterleri escape et
        std::string safe;
        for (char c : msg) {
            if      (c == '"')  safe += "\\\"";
            else if (c == '\\') safe += "\\\\";
            else if (c == '\n') safe += "\\n";
            else if (c == '\r') safe += "\\r";
            else                safe += c;
        }
        std::cout << "{\"ok\":false,\"hata\":\"" << safe << "\","
                  << "\"kod\":" << status << "}\n";
    }
}

// ── File helpers ──────────────────────────────────────────────────────────────

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open: " + path.string());
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// ── Hash ─────────────────────────────────────────────────────────────────────

static std::string file_hash(const fs::path& path, const std::string& content) {
    uint64_t h = 5381;
    for (unsigned char c : content) h = ((h << 5) + h) ^ c;
    auto mtime = fs::last_write_time(path).time_since_epoch().count();
    h ^= (uint64_t)mtime;
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

// ── Cache ─────────────────────────────────────────────────────────────────────

static fs::path cache_dir(const fs::path& script) {
    return script.parent_path() / ".look_cache";
}

static fs::path cache_path(const fs::path& script, const std::string& hash) {
    return cache_dir(script) / (script.stem().string() + "." + hash + ".lkc");
}

static bool cache_valid(const fs::path& cp, const std::string& hash) {
    if (!fs::exists(cp)) return false;
    std::ifstream f(cp);
    std::string stored;
    std::getline(f, stored);
    return stored == hash;
}

static void cache_write(const fs::path& cp, const std::string& hash) {
    try {
        fs::create_directories(cp.parent_path());
        std::ofstream f(cp);
        f << hash << "\n";
    } catch (...) {}
}

// ── Resolve script path ───────────────────────────────────────────────────────

static std::string resolve_script(int argc, char* argv[]) {
    auto ends_lk = [](const char* s) -> bool {
        if (!s || !*s) return false;
        std::string str(s);
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
    if (path_info && *path_info && document_root && *document_root) {
        std::string pi(path_info);
        size_t lk_pos = pi.find(".lk");
        if (lk_pos != std::string::npos) {
            // Script is DOCUMENT_ROOT + /index.lk (up to and including .lk)
            return std::string(document_root) + pi.substr(0, lk_pos + 3);
        }
        return std::string(document_root) + pi;
    }

    if (argc >= 2) return argv[1];
    return "";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string script_path = resolve_script(argc, argv);

    if (script_path.empty()) {
        send_error(400, "Bad Request", "No LOOK script specified.");
        return 1;
    }

    try {
        fs::path script(script_path);

        // ── .env path fix ─────────────────────────────────────────────────────
        // CGI'da working directory Apache'nin kendi klasoru olabilir.
        // .env dosyasini script'in bulundugu klasorde ara.
        fs::path script_dir = script.parent_path();
        if (!script_dir.empty()) {
            // Script dizinine git — env() ve log:: buradan calisacak
#ifdef _WIN32
            _wchdir(script_dir.wstring().c_str());
#else
            (void)chdir(script_dir.string().c_str());
#endif
        }

        // ── Cache ─────────────────────────────────────────────────────────────
        std::string source = read_file(script);
        std::string hash   = file_hash(script, source);
        fs::path    cp     = cache_path(script, hash);

        if (!cache_valid(cp, hash)) {
            cache_write(cp, hash);
            try {
                for (auto& entry : fs::directory_iterator(cache_dir(script))) {
                    auto name = entry.path().stem().string();
                    if (name.find(script.stem().string() + ".") == 0 && entry.path() != cp)
                        fs::remove(entry.path());
                }
            } catch (...) {}
        }

        // ── Web context ───────────────────────────────────────────────────────
        look::WebContext web;
        web.init_from_cgi();

        // ── Parse ─────────────────────────────────────────────────────────────
        look::Lexer  lexer(source);
        auto         tokens  = lexer.scan_tokens();
        look::Parser parser(std::move(tokens));
        std::unique_ptr<look::Program> program;
        try {
            program = parser.parse();
        } catch (const look::LookParseError& e) {
            auto err = e;
            if (err.file.empty()) err.file = script_path;
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI", err.format());
            send_error(500, "Parse Error", err.format());
            return 1;
        }

        // ── Execute ───────────────────────────────────────────────────────────
        std::ostringstream output;
        look::Interpreter interp(output);
        interp.set_web_context(&web);
        interp.set_file(script_path);

        try {
            interp.interpret(*program);
        } catch (const look::RouteMatchedException&) {
            // Normal — route() flow control
        } catch (const look::ExitException&) {
            // exit() / die() — temiz cikis, response gonder
        } catch (const look::LookRuntimeError& e) {
            auto err = e;
            if (err.location.file.empty()) err.location.file = script_path;
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI", err.format());
            send_error(500, "Runtime Error", err.format());
            return 1;
        }

        // ── 404 handler ───────────────────────────────────────────────────────
        // route() kullanilmis ama hicbiri eslesmediyse
        bool used_routing = (source.find("route(") != std::string::npos);
        if (used_routing && !web.route_matched) {
            web.set_status(404);
            // route("404", callback) tanimli mi?
            try {
                look::Value handler = interp.get_global("__404_handler__");
                if (handler.type() == look::Value::FUNCTION) {
                    interp.invoke(handler, {});
                }
            } catch (...) {
                // 404 handler tanimli degil — varsayilan mesaj
                output << "404 Not Found";
            }
        }

        // ── Response ──────────────────────────────────────────────────────────
        std::cout << web.build_headers();
        std::cout << output.str();

    } catch (const std::exception& e) {
        // Production: log'a yaz, kullaniciya temiz sayfa
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI",
            std::string("Uncaught exception: ") + e.what() +
            " | Script: " + script_path);

        send_error(500, "Internal Server Error", e.what());
        return 1;
    }

    return 0;
}

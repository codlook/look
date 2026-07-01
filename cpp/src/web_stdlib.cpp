#define _CRT_RAND_S
#include "look/stdlib.h"
#include "look/web.h"
#include "look/mysql_client.h"
#include "look/sqlite_client.h"
#include "look/postgres_client.h"
#include "look/logger.h"
#include <sstream>
#include <fstream>
#include <set>
#include <stdexcept>
#include <regex>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#ifndef _WIN32
#  include <sys/stat.h>
#endif

namespace look {

// â”€â”€ JSON encode/decode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string json_encode(const Value& v);

static std::string json_encode_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}

static std::string json_encode(const Value& v) {
    switch (v.type()) {
        case Value::INT:    return std::to_string(v.as_int());
        case Value::FLOAT:  { std::ostringstream oss; oss << v.as_float(); return oss.str(); }
        case Value::STRING: return json_encode_string(v.as_string());
        case Value::BOOL:   return v.as_bool() ? "true" : "false";
        case Value::NONE:   return "null";
        case Value::ARRAY: {
            auto& arr = *v.as_array();
            // Associative array / struct: ["__assoc__", k0, v0, k1, v1, ...]
            if (!arr.empty() && arr[0].type() == Value::STRING &&
                arr[0].as_string() == "__assoc__") {
                std::string out = "{";
                bool first = true;
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    // Skip internal runtime tags (__struct__ etc.)
                    if (arr[i].to_string() == "__struct__") continue;
                    if (!first) out += ",";
                    first = false;
                    out += json_encode_string(arr[i].to_string());
                    out += ":";
                    out += json_encode(arr[i + 1]);
                }
                return out + "}";
            }
            // Regular array
            std::string out = "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += json_encode(arr[i]);
            }
            return out + "]";
        }
        default: return "null";
    }
}

// Simple JSON decode â€” returns Value
static Value json_decode_value(const std::string& s, size_t& i);

static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
}

static std::string json_decode_str(const std::string& s, size_t& i) {
    i++; // skip "
    std::string result;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') {
            i++;
            if (i >= s.size()) break;
            switch (s[i]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                default:   result += s[i]; break;
            }
        } else {
            result += s[i];
        }
        i++;
    }
    i++; // skip closing "
    return result;
}

static constexpr int JSON_MAX_DEPTH = 64;

static Value json_decode_value(const std::string& s, size_t& i, int depth = 0) {
    if (depth > JSON_MAX_DEPTH)
        throw std::runtime_error("JSON ayrıştırma hatası: iç içe yapı çok derin (max " + std::to_string(JSON_MAX_DEPTH) + ")");
    json_skip_ws(s, i);
    if (i >= s.size()) return Value();

    if (s[i] == ‘"’) {
        return Value(json_decode_str(s, i));
    }
    // JSON object { } → assoc array
    if (s[i] == ‘{‘) {
        i++;
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        json_skip_ws(s, i);
        while (i < s.size() && s[i] != ‘}’) {
            // key
            std::string key = json_decode_str(s, i);
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ‘:’) i++;
            json_skip_ws(s, i);
            // value
            Value val = json_decode_value(s, i, depth + 1);
            arr->push_back(Value(key));
            arr->push_back(val);
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ‘,’) i++;
            json_skip_ws(s, i);
        }
        if (i < s.size()) i++; // skip }
        return Value(arr);
    }

    if (s[i] == ‘[‘) {
        i++;
        auto arr = std::make_shared<std::vector<Value>>();
        json_skip_ws(s, i);
        while (i < s.size() && s[i] != ‘]’) {
            arr->push_back(json_decode_value(s, i, depth + 1));
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ‘,’) i++;
            json_skip_ws(s, i);
        }
        if (i < s.size()) i++; // skip ]
        return Value(arr);
    }
    if (s.substr(i, 4) == "true")  { i += 4; return Value(true); }
    if (s.substr(i, 5) == "false") { i += 5; return Value(false); }
    if (s.substr(i, 4) == "null")  { i += 4; return Value(); }
    // Number
    size_t start = i;
    bool is_float = false;
    if (s[i] == '-') i++;
    while (i < s.size() && (std::isdigit(s[i]) || s[i]=='.' || s[i]=='e' || s[i]=='E' || s[i]=='+' || s[i]=='-')) {
        if (s[i]=='.' || s[i]=='e' || s[i]=='E') is_float = true;
        i++;
    }
    std::string num = s.substr(start, i - start);
    if (num.empty() || num == "-") return Value(0);
    try {
        if (is_float) return Value(std::stod(num));
        return Value(std::stoi(num));
    } catch (...) { return Value(0); }
}

// â”€â”€ Route matching â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Convert route pattern "/users/{id}" to regex "^/users/([^/]+)$"
// Returns param names in order

static std::regex pattern_to_regex(const std::string& pattern,
                                    std::vector<std::string>& param_names) {
    std::string reg = "^";
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == '{') {
            size_t end = pattern.find('}', i);
            if (end == std::string::npos) break;
            param_names.push_back(pattern.substr(i + 1, end - i - 1));
            reg += "([^/]+)";
            i = end + 1;
        } else if (pattern[i] == '/') {
            reg += "/";
            i++;
        } else {
            // Escape regex special chars
            static const std::string special = ".^$*+?()[]{}|\\";
            if (special.find(pattern[i]) != std::string::npos)
                reg += "\\";
            reg += pattern[i++];
        }
    }
    reg += "$";
    return std::regex(reg);
}

// â”€â”€ Web modules â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static Module make_request(WebContext* ctx) {
    Module m;
    m.name = "request";

    m.functions["method"] = [ctx](auto) -> Value {
        return Value(ctx->method);
    };
    m.functions["path"] = [ctx](auto) -> Value {
        return Value(ctx->path);
    };
    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) {
            // Return all as array
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& [k, v] : ctx->get_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->get_params.find(args[0].to_string());
        return it != ctx->get_params.end() ? Value(it->second) : Value();
    };
    m.functions["post"] = [ctx](auto args) -> Value {
        if (args.empty()) {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& [k, v] : ctx->post_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->post_params.find(args[0].to_string());
        return it != ctx->post_params.end() ? Value(it->second) : Value();
    };
    m.functions["body"] = [ctx](auto) -> Value {
        return Value(ctx->body);
    };
    m.functions["ip"] = [ctx](auto) -> Value {
        return Value(ctx->remote_addr);
    };
    m.functions["is_get"]     = [ctx](auto) -> Value { return Value(ctx->method == "GET");     };
    m.functions["is_post"]    = [ctx](auto) -> Value { return Value(ctx->method == "POST");    };
    m.functions["is_put"]     = [ctx](auto) -> Value { return Value(ctx->method == "PUT");     };
    m.functions["is_delete"]  = [ctx](auto) -> Value { return Value(ctx->method == "DELETE");  };
    m.functions["is_patch"]   = [ctx](auto) -> Value { return Value(ctx->method == "PATCH");   };
    m.functions["is_head"]    = [ctx](auto) -> Value { return Value(ctx->method == "HEAD");    };
    m.functions["is_options"] = [ctx](auto) -> Value { return Value(ctx->method == "OPTIONS"); };
    // request::json() â€” parse JSON body otomatik
    m.functions["json"] = [ctx](auto) -> Value {
        if (ctx->body.empty()) return Value();
        size_t i = 0;
        return json_decode_value(ctx->body, i);
    };
    // request::all() â€” GET + POST params birleÅŸik
    m.functions["all"] = [ctx](auto) -> Value {
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        for (auto& [k, v] : ctx->get_params)  { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        for (auto& [k, v] : ctx->post_params) { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        return Value(arr);
    };
    m.functions["param"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->route_params.find(args[0].to_string());
        return it != ctx->route_params.end() ? Value(it->second) : Value();
    };

    // request::file(name) veya request::file(name, options_array)
    // options keys: max_size (int bytes), allow_mime (array), allow_svg (bool)
    // Varsayılan limit: 1MB
    m.functions["file"] = [ctx](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("request::file() requires field name");

        // multipart/form-data kontrolü
        if (ctx->content_type.find("multipart/form-data") == std::string::npos)
            throw std::runtime_error("request::file() requires multipart/form-data request");

        std::string field = args[0].to_string();
        auto it = ctx->uploaded_files.find(field);
        if (it == ctx->uploaded_files.end() || !it->second.valid)
            return Value(); // dosya seçilmemişse null döndür

        const UploadedFile& uf = it->second;

        // Seçenekleri oku
        size_t    max_size   = 1 * 1024 * 1024; // varsayılan 1MB
        bool      allow_svg  = false;
        std::vector<std::string> allow_mime;

        if (args.size() >= 2 && args[1].type() == Value::ARRAY) {
            auto& arr = *args[1].as_array();
            bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING
                            && arr[0].to_string() == "__assoc__";
            if (is_assoc) {
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    std::string key = arr[i].to_string();
                    Value&      val = arr[i+1];
                    if (key == "max_size")  max_size  = (size_t)val.to_int();
                    if (key == "allow_svg") allow_svg = val.is_truthy();
                    if (key == "allow_mime" && val.type() == Value::ARRAY) {
                        for (auto& v : *val.as_array()) {
                            if (v.type() != Value::STRING || v.to_string() == "__assoc__") continue;
                            allow_mime.push_back(v.to_string());
                        }
                    }
                }
            }
        }

        // Boyut kontrolü
        if (uf.size > max_size)
            throw std::runtime_error("Uploaded file exceeds max_size limit (" +
                                     std::to_string(max_size) + " bytes)");

        // SVG kontrolü — allow_mime içinde SVG varsa allow_svg de zorunlu
        if (uf.mime == "image/svg+xml" && !allow_svg)
            throw std::runtime_error("SVG upload requires allow_svg: true option");

        // SVG XSS sanitizasyonu — whitelist tabanlı element + attribute parser
        // Blacklist yaklaşımı yerine: sadece bilinen güvenli elementlere ve attribute'lara izin ver.
        if (uf.mime == "image/svg+xml" && allow_svg) {
            std::ifstream svg_in(uf.temp_path, std::ios::binary);
            if (svg_in) {
                std::string content((std::istreambuf_iterator<char>(svg_in)),
                                     std::istreambuf_iterator<char>());
                svg_in.close();

                content = look::svg_sanitize(content);

                std::ofstream svg_out(uf.temp_path, std::ios::binary | std::ios::trunc);
                if (svg_out) svg_out.write(content.data(), (std::streamsize)content.size());
            }
        }

        // MIME kontrolü — allow_mime belirtilmişse listede olmalı
        if (!allow_mime.empty()) {
            bool found = false;
            for (auto& m : allow_mime) if (m == uf.mime) { found = true; break; }
            if (!found)
                throw std::runtime_error("File type not allowed: " + uf.mime);
        }

        // Dönüş değeri — assoc array
        auto result = std::make_shared<std::vector<Value>>();
        result->push_back(Value(std::string("__assoc__")));
        result->push_back(Value(std::string("path")));   result->push_back(Value(uf.temp_path));
        result->push_back(Value(std::string("mime")));   result->push_back(Value(uf.mime));
        result->push_back(Value(std::string("size")));   result->push_back(Value((int)uf.size));
        result->push_back(Value(std::string("sha256"))); result->push_back(Value(uf.sha256));
        return Value(result);
    };

    return m;
}

static Module make_response_module(WebContext* ctx) {
    Module m;
    m.name = "response";

    m.functions["status"] = [ctx](auto args) -> Value {
        if (!args.empty()) ctx->set_status(args[0].to_int());
        return Value(ctx->status_code);
    };
    m.functions["header"] = [ctx](auto args) -> Value {
        if (args.size() >= 2)
            ctx->headers_out[args[0].to_string()] = args[1].to_string();
        return Value();
    };
    m.functions["redirect"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->set_status(args.size() >= 2 ? args[1].to_int() : 302);
        ctx->headers_out["Location"] = args[0].to_string();
        return Value();
    };
    m.functions["json"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "application/json; charset=utf-8";
        if (!args.empty()) return Value(json_encode(args[0]));
        return Value();
    };

    return m;
}

static Module make_json_module() {
    Module m;
    m.name = "json";

    m.functions["encode"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string("null"));
        return Value(json_encode(args[0]));
    };
    m.functions["decode"] = [](auto args) -> Value {
        if (args.empty()) return Value();
        std::string s = args[0].to_string();
        size_t i = 0;
        return json_decode_value(s, i);
    };

    return m;
}

static Module make_cookie_module(WebContext* ctx) {
    Module m;
    m.name = "cookie";

    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->cookies_in.find(args[0].to_string());
        return it != ctx->cookies_in.end() ? Value(it->second) : Value();
    };
    m.functions["set"] = [ctx](auto args) -> Value {
        if (args.size() < 2) return Value();
        std::string name    = args[0].to_string();
        std::string value   = args[1].to_string();
        int expires         = args.size() >= 3 ? args[2].to_int() : 0;
        std::string path_c  = args.size() >= 4 ? args[3].to_string() : "/";

        std::string cookie = name + "=" + value + "; Path=" + path_c;
        if (expires > 0) {
            time_t exp = std::time(nullptr) + expires;
            char buf[64];
            struct tm tm_buf{};
#ifdef _WIN32
            gmtime_s(&tm_buf, &exp);
#else
            gmtime_r(&exp, &tm_buf);
#endif
            strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
            cookie += "; Expires=" + std::string(buf);
        }
        // Multiple Set-Cookie headers â€” append with unique key
        ctx->headers_out["Set-Cookie"] = cookie;
        return Value();
    };
    m.functions["delete"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->headers_out["Set-Cookie"] =
            args[0].to_string() + "=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
        return Value();
    };
    m.functions["has"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value(false);
        return Value(ctx->cookies_in.count(args[0].to_string()) > 0);
    };

    return m;
}

static Module make_session_module(WebContext* ctx) {
    Module m;
    m.name = "session";

    // File-based sessions — stored in system temp dir
    // Cookie değeri sadece hex karakterleri içermelidir (path traversal önlemi)
    auto session_id_valid = [](const std::string& sid) -> bool {
        if (sid.empty() || sid.size() > 64) return false;
        for (char c : sid)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        return true;
    };
    auto session_file = [ctx, session_id_valid]() -> std::string {
        auto it = ctx->cookies_in.find(“LOOK_SESSION”);
        if (it == ctx->cookies_in.end()) return “”;
        if (!session_id_valid(it->second)) return “”;  // path traversal engeli
        return std::string(std::getenv(“TEMP”) ? std::getenv(“TEMP”) : “/tmp”) +
               “/look_sess_” + it->second;
    };

    m.functions["start"] = [ctx, session_file](auto) -> Value {
        // Generate session ID if not exists
        if (!ctx->cookies_in.count("LOOK_SESSION")) {
            // Cryptographically secure random bytes — /dev/urandom (Linux) or rand_s (Windows)
            char id[33];
#if defined(_WIN32)
            for (int i = 0; i < 32; i++) {
                unsigned int r = 0;
                rand_s(&r);
                id[i] = "0123456789abcdef"[r % 16];
            }
#else
            {
                std::ifstream urandom("/dev/urandom", std::ios::binary);
                uint8_t bytes[16];
                urandom.read(reinterpret_cast<char*>(bytes), 16);
                for (int i = 0; i < 16; i++) {
                    id[i*2]   = "0123456789abcdef"[(bytes[i] >> 4) & 0xf];
                    id[i*2+1] = "0123456789abcdef"[bytes[i] & 0xf];
                }
            }
#endif
            id[32] = 0;
            std::string sid(id);
            ctx->cookies_in["LOOK_SESSION"] = sid;
            ctx->headers_out["Set-Cookie"] = "LOOK_SESSION=" + sid + "; Path=/; HttpOnly; Secure; SameSite=Lax";
        }
        return Value(ctx->cookies_in["LOOK_SESSION"]);
    };
    m.functions["set"] = [ctx, session_file](auto args) -> Value {
        if (args.size() < 2) return Value();
        auto sf = session_file();
        if (sf.empty()) return Value();
        std::ofstream f(sf, std::ios::app);
#ifndef _WIN32
        // 0600 izni: paylaşımlı sistemlerde diğer kullanıcılar okuyamaz
        chmod(sf.c_str(), 0600);
#endif
        f << args[0].to_string() << "=" << args[1].to_string() << "\n";
        return Value();
    };
    m.functions["get"] = [ctx, session_file](auto args) -> Value {
        if (args.empty()) return Value();
        auto sf = session_file();
        if (sf.empty()) return Value();
        std::ifstream f(sf);
        std::string line;
        std::string key = args[0].to_string();
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) == key)
                return Value(line.substr(eq + 1));
        }
        return Value();
    };
    m.functions["destroy"] = [ctx, session_file](auto) -> Value {
        auto sf = session_file();
        if (!sf.empty()) std::remove(sf.c_str());
        ctx->cookies_in.erase("LOOK_SESSION");  // Sonraki session::start() yeni ID üretsin
        ctx->headers_out["Set-Cookie"] =
            "LOOK_SESSION=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Secure; SameSite=Lax";
        return Value();
    };

    return m;
}

// â”€â”€ Route module â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// route() is handled as a built-in function in the interpreter.
// This module provides route::params() helper.

static Module make_route_module(WebContext* ctx) {
    Module m;
    m.name = "route";

    m.functions["param"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->route_params.find(args[0].to_string());
        return it != ctx->route_params.end() ? Value(it->second) : Value();
    };
    m.functions["matched"] = [ctx](auto) -> Value {
        return Value(ctx->route_matched);
    };

    return m;
}

// â”€â”€ db module â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// db::connect("mysql://user:pass@host:port/database")
// db::query($conn, "SELECT ...", [$param1, $param2])
// db::exec($conn, "INSERT ...", [$param1])
// db::escape($conn, $str)
// db::last_id($conn)
// db::affected($conn)

static std::string parse_dsn_part(const std::string& dsn,
                                   std::string& user, std::string& pass,
                                   std::string& host, int& port, std::string& db) {
    // mysql://user:pass@host:port/database
    size_t scheme_end = dsn.find("://");
    if (scheme_end == std::string::npos) throw std::runtime_error("db: invalid DSN format");
    std::string scheme = dsn.substr(0, scheme_end);
    std::string rest   = dsn.substr(scheme_end + 3);

    // user:pass@host:port/db
    size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        size_t colon = userinfo.find(':');
        if (colon != std::string::npos) { user = userinfo.substr(0, colon); pass = userinfo.substr(colon + 1); }
        else user = userinfo;
    }
    size_t slash = rest.find('/');
    std::string hostport = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    db = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = std::stoi(hostport.substr(colon + 1)); }
    else host = hostport;

    return scheme;
}

// ── Connection pool — thread-safe, per-request borrow/return ──────────────────

struct ConnPool {
    std::string                                dsn;
    std::vector<std::shared_ptr<DbConnection>> all;
    std::vector<std::shared_ptr<DbConnection>> available;
    std::mutex                                 mtx;
    std::condition_variable                    cv;

    std::shared_ptr<DbConnection> acquire() {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this] { return !available.empty(); });
        auto c = available.back(); available.pop_back();
        return c;
    }
    void release(std::shared_ptr<DbConnection> c) {
        { std::lock_guard<std::mutex> lk(mtx); available.push_back(c); }
        cv.notify_one();
    }
};

static std::map<std::string, std::shared_ptr<ConnPool>> g_pools;
static std::mutex                                        g_pools_mtx;
static int                                               g_pool_seq  = 0;
static std::atomic<int>                                  g_pool_size{0}; // 0 = auto

// Per-request, per-thread connection map: pool_key → borrowed conn
static thread_local std::map<std::string, std::shared_ptr<DbConnection>> tl_conns;

void set_db_pool_size(int n) { g_pool_size.store(n > 0 ? n : 0); }

void acquire_thread_connections() {
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    for (auto& [key, pool] : g_pools)
        tl_conns[key] = pool->acquire();
}
void release_thread_connections() {
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        for (auto& [key, conn] : tl_conns)
            if (auto it = g_pools.find(key); it != g_pools.end())
                it->second->release(conn);
    }
    tl_conns.clear();
}

void clear_db_pools() {
    // Hot reload öncesi çağrılır — tüm poolları kapat (exclusive lock altında)
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    g_pools.clear();
    // g_pool_seq sıfırlanmaz: yeni pool'lar yeni key alır, eski closure'lar stale olmaz
    // (route_registry yeniden kurulur zaten)
}

// get_conn — returns the thread-local connection for this request
static std::shared_ptr<DbConnection> get_conn(const Value& v) {
    if (v.type() != Value::STRING)
        throw std::runtime_error("db: invalid connection handle");
    const std::string& key = v.as_string();
    // Thread-local (dispatch mode): per-request borrowed connection
    auto tl_it = tl_conns.find(key);
    if (tl_it != tl_conns.end()) return tl_it->second;
    // Fallback: single-thread setup mode or direct-mode scripts
    std::shared_ptr<ConnPool> pool;
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        auto it = g_pools.find(key);
        if (it == g_pools.end())
            throw std::runtime_error("db: connection not found");
        pool = it->second;
    }
    // acquire with auto-release deleter so setup-mode queries don't starve the pool
    auto raw = pool->acquire();
    return std::shared_ptr<DbConnection>(raw.get(), [pool, raw](DbConnection*) mutable {
        pool->release(raw);
    });
}

// pool_key — sequential handle string
static std::string make_pool_key() {
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    return "__pool_" + std::to_string(g_pool_seq++);
}

// open one physical connection for a given DSN
static std::shared_ptr<DbConnection> open_one_connection(
        const std::string& dsn, const std::vector<Value>& args) {
    if (dsn.substr(0, 9) == "sqlite://") {
        std::string path = dsn.substr(9);
        auto c = std::make_shared<SqliteClient>();
        c->open(path);
        return c;
    }
    if (dsn.substr(0, 11) == "postgres://" || dsn.substr(0, 14) == "postgresql://") {
        std::string user, pass, host = "127.0.0.1", db_name;
        int port = 5432;
        parse_dsn_part(dsn, user, pass, host, port, db_name);
        auto c = std::make_shared<PostgresClient>();
        c->connect(host, port, user, pass, db_name);
        return c;
    }
    std::string user, pass, host = "127.0.0.1", db_name;
    int port = 3306;
    std::string scheme = parse_dsn_part(dsn, user, pass, host, port, db_name);
    if (scheme != "mysql" && scheme != "mariadb")
        throw std::runtime_error("db::connect() unsupported DSN scheme: " + scheme);
    DbConfig cfg;
    cfg.host = host; cfg.port = port; cfg.user = user;
    cfg.password = pass; cfg.database = db_name;
    if (args.size() >= 2 && args[1].type() == Value::ARRAY) {
        auto& arr = *args[1].as_array();
        for (size_t i = 1; i + 1 < arr.size(); i += 2) {
            std::string k = arr[i].to_string();
            if (k == "timeout" || k == "connect_timeout") cfg.connect_timeout_ms = arr[i+1].to_int();
            if (k == "query_timeout")                      cfg.query_timeout_ms   = arr[i+1].to_int();
            if (k == "reconnect"  || k == "max_reconnect") cfg.max_reconnect      = arr[i+1].to_int();
        }
    }
    auto c = std::make_shared<MySQLClient>();
    c->connect(cfg);
    return c;
}

// Convert DbRow to LOOK array value
static Value row_to_value(const DbRow& row) {
    auto arr = std::make_shared<std::vector<Value>>();
    for (auto& [k, dv] : row) {
        arr->push_back(Value(dv.str));
    }
    return Value(arr);
}

// Convert DbRow to associative array (Value array of [key, value] pairs)
// Since LOOK doesn't have maps yet, we return as positional array
// matching column order, and also expose ::row_get() helper
// MySQL integer column types
static bool mysql_is_int(uint8_t t) {
    return t == 0x01 || t == 0x02 || t == 0x03 ||
           t == 0x08 || t == 0x09 || t == 0x0D;
}
// MySQL floating-point / decimal column types
static bool mysql_is_float(uint8_t t) {
    return t == 0x04 || t == 0x05 || t == 0xF6;
}
// SQLite type sabitleri
static bool sqlite_is_int(uint8_t t)   { return t == sqlite_type::INTEGER; }
static bool sqlite_is_float(uint8_t t) { return t == sqlite_type::FLOAT; }
static bool sqlite_is_null(uint8_t t)  { return t == sqlite_type::NUL; }
// PostgreSQL type sabitleri
static bool pg_is_int(uint8_t t)       { return t == pg_type::INT; }
static bool pg_is_float(uint8_t t)     { return t == pg_type::FLOAT; }
static bool pg_is_null(uint8_t t)      { return t == pg_type::NUL; }
static bool pg_is_bool(uint8_t t)      { return t == pg_type::BOOL; }

static Value rows_to_value(const std::vector<DbRow>& rows) {
    auto result = std::make_shared<std::vector<Value>>();
    for (const auto& row : rows) {
        // Store as assoc array: ["__assoc__", col0, val0, col1, val1, ...]
        auto row_arr = std::make_shared<std::vector<Value>>();
        row_arr->push_back(Value(std::string("__assoc__")));
        for (auto& [k, dv] : row) {
            row_arr->push_back(Value(k));
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) {
                row_arr->push_back(Value());  // null
            } else if (pg_is_bool(dv.type)) {
                // PostgreSQL BOOL: "t" = true, "f" = false
                row_arr->push_back(Value(dv.str == "t" || dv.str == "true" || dv.str == "1"));
            } else if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { row_arr->push_back(Value((int)std::stoll(dv.str))); }
                catch (...) { row_arr->push_back(Value(dv.str)); }
            } else if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                try { row_arr->push_back(Value(std::stod(dv.str))); }
                catch (...) { row_arr->push_back(Value(dv.str)); }
            } else {
                row_arr->push_back(Value(dv.str));
            }
        }
        result->push_back(Value(row_arr));
    }
    return Value(result);
}

// SQL injection koruması — driver'a göre doğru escape
static std::string escape_str(const std::string& s, const char* driver) {
    if (strcmp(driver, "mysql") == 0 || strcmp(driver, "mariadb") == 0)
        return MySQLClient::escape(s);
    // SQLite ve PostgreSQL: standart SQL, tek tırnak '' ile escape
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else           out += c;
    }
    return out;
}

// LOOK Value → DbParam (driver-agnostik, gerçek prepared statement parametresi)
static DbParam value_to_param(const Value& v) {
    switch (v.type()) {
        case Value::NONE:  return DbParam::null();
        case Value::INT:   return DbParam::from_int(v.as_int());
        case Value::FLOAT: return DbParam::from_float(v.as_float());
        case Value::BOOL:  return DbParam::from_bool(v.as_bool());
        default:           return DbParam::from_text(v.to_string());
    }
}
static std::vector<DbParam> values_to_params(const std::vector<Value>& vals) {
    std::vector<DbParam> out;
    out.reserve(vals.size());
    for (const auto& v : vals) out.push_back(value_to_param(v));
    return out;
}

static Module make_db_module() {
    Module m;
    m.name = "db";

    // db::connect("mysql://user:pass@host/database")
    // db::connect("sqlite://blog.db")
    // Creates a connection pool; pool size = --workers (default: hardware_concurrency)
    m.functions["connect"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::connect() requires DSN string");
        std::string dsn = args[0].to_string();

        bool is_sqlite = (dsn.substr(0, 9) == "sqlite://");
        int sz = g_pool_size.load();
        if (sz <= 0) sz = (int)std::thread::hardware_concurrency();
        if (sz < 2)  sz = 2;
        if (is_sqlite) { if (sz > 4) sz = 4; if (sz < 2) sz = 2; }  // SQLite: WAL concurrent reads, up to 4

        auto pool = std::make_shared<ConnPool>();
        pool->dsn = dsn;
        for (int i = 0; i < sz; i++) {
            auto c = open_one_connection(dsn, args);
            pool->all.push_back(c);
            pool->available.push_back(c);
        }

        std::string key = make_pool_key();
        {
            std::lock_guard<std::mutex> lk(g_pools_mtx);
            g_pools[key] = pool;
        }
        Logger::instance().log(LogLevel::LOG_INFO, "DB",
            "Pool[" + std::to_string(sz) + "] created: " +
            dsn.substr(0, dsn.find('@') == std::string::npos ? dsn.size() : dsn.find('@')));
        return Value(key);
    };

    // db::query($conn, "SELECT ...", [$p1, $p2])
    m.functions["query"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::query() requires connection and SQL");
        auto conn = get_conn(args[0]);
        std::string sql = args[1].to_string();
        std::vector<Value> params;
        if (args.size() >= 3 && args[2].type() == Value::ARRAY)
            params = *args[2].as_array();
        auto rows = conn->execute(sql, values_to_params(params));
        return rows_to_value(rows);
    };

    // db::exec($conn, "INSERT/UPDATE/DELETE ...", [$p1])
    m.functions["exec"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::exec() requires connection and SQL");
        auto conn = get_conn(args[0]);
        std::string sql = args[1].to_string();
        std::vector<Value> params;
        if (args.size() >= 3 && args[2].type() == Value::ARRAY)
            params = *args[2].as_array();
        conn->execute(sql, values_to_params(params));
        return Value((int)conn->affected_rows());
    };

    // db::last_id($conn) — uses thread-local connection (same as exec in this request)
    m.functions["last_id"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::last_id() requires connection");
        return Value((int)get_conn(args[0])->last_insert_id());
    };

    // db::affected($conn)
    m.functions["affected"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::affected() requires connection");
        return Value((int)get_conn(args[0])->affected_rows());
    };

    // db::escape($conn, $str)
    m.functions["escape"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::escape() requires connection and string");
        return Value(MySQLClient::escape(args[1].to_string()));
    };

    // db::close($conn) — removes the pool; existing borrowed conns complete normally
    m.functions["close"] = [](auto args) -> Value {
        if (args.empty()) return Value();
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        g_pools.erase(args[0].to_string());
        return Value();
    };

    // db::col($conn, $sql, $params) → ilk satırın ilk kolonu
    // db::col($row, $index_or_name) → row'dan kolon değeri
    m.functions["col"] = [](auto args) -> Value {
        if (args.size() < 2) return Value();

        // Bağlantı + SQL çalıştırma modu
        auto is_pool_handle = [](const Value& v) {
            if (v.type() != Value::STRING) return false;
            const auto& s = v.as_string();
            return s.substr(0, 7) == "__pool_";
        };
        if (is_pool_handle(args[0])) {
            auto conn = get_conn(args[0]);
            std::string sql = args[1].to_string();
            std::vector<Value> params;
            if (args.size() >= 3 && args[2].type() == Value::ARRAY)
                params = *args[2].as_array();
            auto rows = conn->execute(sql, values_to_params(params));
            if (rows.empty() || rows[0].empty()) return Value();
            // İlk satırın ilk kolonunu dön
            const auto& dv = rows[0][0].second;
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) return Value();
            if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { return Value((int)std::stoll(dv.str)); } catch (...) {}
            }
            if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                try { return Value(std::stod(dv.str)); } catch (...) {}
            }
            return Value(dv.str);
        }

        if (args[0].type() != Value::ARRAY) return Value();
        auto& arr = *args[0].as_array();

        // Assoc array row: ["__assoc__", col0, val0, col1, val1, ...]
        if (!arr.empty() && arr[0].type() == Value::STRING &&
            arr[0].as_string() == "__assoc__") {
            // String key: find by name
            if (args[1].type() == Value::STRING) {
                const std::string& key = args[1].as_string();
                for (size_t i = 1; i + 1 < arr.size(); i += 2)
                    if (arr[i].to_string() == key) return arr[i + 1];
                return Value();
            }
            // Numeric index: 0 = first column value
            int idx = args[1].to_int();
            size_t val_pos = 2 + idx * 2; // skip __assoc__ + key, land on value
            if (val_pos < arr.size()) return arr[val_pos];
            return Value();
        }

        // Plain array
        int idx = args[1].to_int();
        if (idx < 0 || idx >= (int)arr.size()) return Value();
        return arr[idx];
    };

    return m;
}

// â”€â”€ Export â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::map<std::string, Module> make_web_modules(WebContext* ctx) {
    std::map<std::string, Module> mods;
    auto add = [&](Module mod) { mods[mod.name] = std::move(mod); };
    add(make_request(ctx));
    add(make_response_module(ctx));
    add(make_json_module());
    add(make_cookie_module(ctx));
    add(make_session_module(ctx));
    add(make_route_module(ctx));
    add(make_db_module());
    return mods;
}

} // namespace look


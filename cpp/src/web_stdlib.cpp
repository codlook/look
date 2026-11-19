#define _CRT_RAND_S
#include "look/mem_budget.h"   // mem_reserve — budget before a large allocation
#include "look/stdlib.h"
#include "look/web.h"
#include "look/struct_types.h"
#include "look/db_dsn.h"   // pg_resolve_tls (pure TLS-decision seam)
#include "look/host_port.h" // host[:port] + port parsing (Go net/url model; pure, table-tested)
#include "look/session_files.h" // file driver: 0700 directory, 0600 file, ownership check
#include "look/crypto_sha256.h"  // csrf:: HMAC-SHA256
#include "look/session_esc.h"   // valid_sid + sess_esc/blob (pure, table-testable)
#include "look/fiber.h"
#include "look/request_scope.h"
#include <cmath>
#include <cstdint>
#include "look/runtime_host.h"
#include "look/mysql_client.h"
#include "look/sqlite_client.h"
#include "look/postgres_client.h"
#include "look/resp_client.h"
#include "look/service_error.h"
#include "look/logger.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <regex>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <condition_variable>
#include <thread>
#ifdef __linux__
#include <sched.h>   // sched_getaffinity — available_cpus
#endif
#include <atomic>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include "look/json_str_parse.h"   // pure JSON string-unescape (the single definition shared with fuzz)
#include "look/parse_double.h"
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace look {

// â”€â”€ JSON encode/decode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string json_encode(const Value& v, int depth = 0);

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

static std::string json_encode(const Value& v, int depth) {
    // Depth: on the array branch the language's common limit (NestingGuard, LOOK_MAX_NESTING = 1000) → an explicit,
    // catchable error; a cyclic structure ends with the same error. OLD: anything deeper than 256 silently
    // wrote "null" — the data reached the client corrupted (Go json.Marshal never silently
    // drops a value; PHP json_encode errors on exceeding the depth). `depth` remains only for signature compatibility.
    (void)depth;
    switch (v.type()) {
        case Value::INT:    return std::to_string(v.as_int());
        case Value::FLOAT:  {
            double d = v.as_float();
            // NaN/Infinity do not exist in JSON: an explicit error (Go json.Marshal: "unsupported value"). It used to
            // silently write null — an overflowing computation (1e308 * 10) reached the client as "no value".
            if (!std::isfinite(d))
                throw std::runtime_error(std::string("json::encode: ") + (std::isnan(d) ? "NaN" : "Infinity") +
                    " cannot be written as JSON (check the calculation, or encode it as a string)");
            return look_format_double(d);
        }
        case Value::STRING: return json_encode_string(v.as_string());
        case Value::BOOL:   return v.as_bool() ? "true" : "false";
        case Value::NONE:   return "null";
        // struct → an object with field names (declaration order).
        case Value::STRUCT: return json_encode(look::struct_to_map(v), depth);
        case Value::ARRAY: {
            NestingGuard g;
            auto& arr = *v.as_array();
            // Associative array / struct: ["__assoc__", k0, v0, k1, v1, ...]
            if (!arr.empty() && arr[0].is_assoc_marker()) {
                std::string out = "{";
                bool first = true;
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    // Skip internal runtime tags (__struct__ etc.)
                    if (arr[i].to_string() == "__struct__") continue;
                    if (!first) out += ",";
                    first = false;
                    out += json_encode_string(arr[i].to_string());
                    out += ":";
                    out += json_encode(arr[i + 1], depth + 1);
                }
                return out + "}";
            }
            // Regular array
            std::string out = "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += json_encode(arr[i], depth + 1);
            }
            return out + "]";
        }
        default: return "null";
    }
}

// Simple JSON decode â€” returns Value
// Depth limit — deeply nested JSON ([[[[...]]]] or {...}) overflows the C++ stack
// and causes SIGSEGV/a worker crash. Since request::json() decodes the HTTP body
// this is a network-facing DoS. 256 is more than enough for real JSON.
static constexpr int JSON_MAX_DEPTH = 256;
static Value json_decode_value(const std::string& s, size_t& i, int depth = 0);

static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
}


// When the depth is exceeded the WHOLE parse is aborted (nulling only that branch is not enough:
// since position i does not advance, the outer loop would push null forever and cause bad_alloc/
// a hang). It is caught at the public entry points → a clean null is returned.
struct JsonTooDeep {};
// Malformed JSON (a character that makes no progress) → abort the parse. Otherwise in the array/
// object loop `i` does not advance and there is an INFINITE LOOP (a remote DoS
// that needs no auth: a single `[a]` request locks the worker at 100% CPU). It is
// caught at the public entry points → a clean null is returned.
struct JsonParseError {};

// Strict grammar (RFC 8259, Go encoding/json): ':' and ',' are required, no trailing comma, number
// -?(0|[1-9]d*)(.d+)?([eE][+-]?d+)?, literals exactly "true"/"false"/"null". OLD LOOSENESS (measured):
// {"a" 1}, [1 2 3], [1,], {"a":1,}, 01 were accepted — a gateway and LOOK read the same body
// differently (JSON parsing discrepancy). Trailing garbage is rejected at the public entry point (json_decode_document).
static Value json_decode_value(const std::string& s, size_t& i, int depth) {
    if (depth > JSON_MAX_DEPTH) throw JsonTooDeep{};  // stack + heap DoS defense
    json_skip_ws(s, i);
    if (i >= s.size()) throw JsonParseError{};

    if (s[i] == '"') {
        std::string str;
        if (!json_decode_str(s, i, str)) throw JsonParseError{};
        return Value(std::move(str));
    }
    // JSON object { } → assoc array
    if (s[i] == '{') {
        i++;
        auto arr = std::make_shared<ValueArray>();
        arr->push_back(Value::assoc_marker());
        // Key → value position. OLD: on every key all the previous keys were scanned (O(n²)):
        // a 1 MB body with 100k keys cost 50 s of CPU in a single request (measured) — an unauthenticated remote DoS.
        // On a small object (most real bodies) a linear scan is cheap and allocation-free; the index is built only when
        // the SMALL key count is exceeded → as fast as before on a small object, O(n) on a large one.
        constexpr size_t SMALL = 16;
        std::unordered_map<std::string, size_t> pos;   // no allocation while empty
        auto find_key = [&](const std::string& key) -> size_t {   // value position; 0 = absent
            size_t nkeys = (arr->size() - 1) / 2;
            if (nkeys <= SMALL) {
                for (size_t k = 1; k + 1 < arr->size(); k += 2)
                    if ((*arr)[k].as_string() == key) return k + 1;
                return 0;
            }
            if (pos.empty())   // when the threshold is first exceeded, build from the existing keys
                for (size_t k = 1; k + 1 < arr->size(); k += 2) pos.emplace((*arr)[k].as_string(), k + 1);
            auto it = pos.find(key);
            return it == pos.end() ? 0 : it->second;
        };
        json_skip_ws(s, i);
        if (i < s.size() && s[i] == '}') { i++; return Value(arr); }
        for (;;) {
            json_skip_ws(s, i);
            if (i >= s.size() || s[i] != '"') throw JsonParseError{};
            std::string key;
            if (!json_decode_str(s, i, key)) throw JsonParseError{};
            json_skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') throw JsonParseError{};
            i++;
            Value val = json_decode_value(s, i, depth + 1);
            // B-04: duplicate key → the LAST wins, the key POSITION is kept
            // (same as JS/Go/Python/PHP). The old behavior was "the first wins + keep both"
            // → a gateway sees "user" and lets it through, the backend sees "admin" (JSON interop
            // authorization bypass). Update in place: so the key does not move to the end (that is a mismatch too).
            if (size_t at = find_key(key)) (*arr)[at] = std::move(val);
            else {
                if (!pos.empty()) pos.emplace(key, arr->size() + 1);
                arr->push_back(Value(std::move(key))); arr->push_back(std::move(val));
            }
            json_skip_ws(s, i);
            if (i >= s.size()) throw JsonParseError{};
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return Value(arr); }
            throw JsonParseError{};
        }
    }

    if (s[i] == '[') {
        i++;
        auto arr = std::make_shared<ValueArray>();
        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ']') { i++; return Value(arr); }
        for (;;) {
            arr->push_back(json_decode_value(s, i, depth + 1));
            json_skip_ws(s, i);
            if (i >= s.size()) throw JsonParseError{};
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; return Value(arr); }
            throw JsonParseError{};
        }
    }
    if (s.compare(i, 4, "true") == 0)  { i += 4; return Value(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return Value(false); }
    if (s.compare(i, 4, "null") == 0)  { i += 4; return Value(); }
    // Number: -?(0|[1-9]d*)(.d+)?([eE][+-]?d+)?
    size_t start = i;
    bool is_float = false;
    auto digit = [&](size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; };
    if (i < s.size() && s[i] == '-') i++;
    if (!digit(i)) throw JsonParseError{};
    if (s[i] == '0') { i++; if (digit(i)) throw JsonParseError{}; }   // no leading zero ("01")
    else while (digit(i)) i++;
    if (i < s.size() && s[i] == '.') {
        is_float = true; i++;
        if (!digit(i)) throw JsonParseError{};
        while (digit(i)) i++;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        is_float = true; i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        if (!digit(i)) throw JsonParseError{};
        while (digit(i)) i++;
    }
    std::string num = s.substr(start, i - start);
    if (is_float) {
        // 1e-400 / 5e-324 are valid (nearest value, as Go); only an overflow (1e999) makes the document
        // invalid — Go: "number out of range". std::stod rejected both, so a valid document was null.
        double d;
        if (!parse_double(num, d)) throw JsonParseError{};
        return Value(d);
    }
    try {
        return Value((int64_t)std::stoll(num));   // 32-bit stoi overflowed and returned 0
    } catch (...) {
        // An integer BEYOND the int64 range: falling to float is a SILENT loss of precision.
        // 9223372036854775809 → 9223372036854775808 (1 less), uint64-max
        // 18446744073709551615 → ...616 (1 more). Snowflake/Discord/Twitter IDs,
        // MySQL BIGINT UNSIGNED keys are in this range — the ID is silently corrupted, the wrong
        // record matches. Since `...808` looks like a reasonable integer it is far more
        // dangerous than `1e+27`. PRESERVE PRECISION: return the original text as a STRING
        // (LOOK has no uint64/bignum type; a string carries the exact ID losslessly and
        // goes to the DB/comparison one to one). The float form (is_float) already
        // accepts the loss of precision, so it stays double.
        return Value(num);
    }
}

// Document = a single value + only whitespace. OLD: trailing garbage was ignored ("[1] xyz" → [1]).
// Invalid / too deep document → null (the json::decode and request::json contract).
std::string json_encode_value(const Value& v) { return json_encode(v); }

static Value json_decode_document_here(const std::string& s);
// A large document is decoded in the shared pool inside a fiber (fiber.h fiber_offload): if long C++ work that cannot be split
// holds the thread, the other requests on the same thread wait until it finishes.
static Value json_decode_document(const std::string& s) {
    // Measured (16 CPU, while 4 × 12 MB decodes ran): keep-alive /ok p99 bound to the same thread
    // 99 ms → 0.7 ms. A small document is decoded here (the cost of moving to the pool is greater than the gain).
    if (s.size() >= (size_t(256) << 10)) return fiber_offload([&] { return json_decode_document_here(s); });
    return json_decode_document_here(s);
}
static Value json_decode_document_here(const std::string& s) {
    size_t i = 0;
    try {
        Value v = json_decode_value(s, i);
        json_skip_ws(s, i);
        if (i != s.size()) return Value();
        return v;
    }
    catch (const JsonTooDeep&) { return Value(); }   // too deep → null (DoS defense)
    catch (const JsonParseError&) { return Value(); } // malformed → null
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

// ── Request handle (look/request_scope.h) ───────────────────────────────────
// fiber-ok: used only OUTSIDE a fiber (if Fiber::current() exists, the fiber's own field)
static thread_local WebContext*   tl_req_web = nullptr;
static thread_local std::ostream* tl_req_out = nullptr;   // fiber-ok: same rule as the one above

WebContext* current_request_web() {
    if (Fiber* f = Fiber::current()) return static_cast<WebContext*>(f->local.req_web);
    return tl_req_web;
}
std::ostream* current_request_out() {
    if (Fiber* f = Fiber::current()) return static_cast<std::ostream*>(f->local.req_out);
    return tl_req_out;
}
RequestScope::RequestScope(WebContext* web, std::ostream* out)
    : prev_web_(current_request_web()), prev_out_(current_request_out()) {
    if (Fiber* f = Fiber::current()) { f->local.req_web = web; f->local.req_out = out; }
    else { tl_req_web = web; tl_req_out = out; }
}
RequestScope::~RequestScope() {
    if (Fiber* f = Fiber::current()) { f->local.req_web = prev_web_; f->local.req_out = prev_out_; }
    else { tl_req_web = prev_web_; tl_req_out = prev_out_; }
}

// The modules' context handle. If fixed is set, that context (command line, setup, job queue — single-
// context paths); if empty, the context of the request at call time (web dispatch). If called with no request, an
// explicit error — instead of writing to a dead address.
struct WebRef {
    WebContext* fixed = nullptr;
    WebContext* get() const {
        if (fixed) return fixed;
        WebContext* w = current_request_web();
        if (!w) throw std::runtime_error("request data is not available here: this runs outside a request");
        return w;
    }
    WebContext* operator->() const { return get(); }
    WebContext& operator*() const { return *get(); }
    operator WebContext*() const { return get(); }
};

static Module make_request(WebRef ctx) {
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
            auto arr = std::make_shared<ValueArray>();
            for (auto& [k, v] : ctx->get_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->get_params.find(args[0].to_string());
        // The second argument is the DEFAULT value — the same pattern as env(key,default) / config(key,default).
        // OLD BUG: the second argument was SILENTLY IGNORED;
        // a developer who wrote request::get("page", 1) got null.
        if (it != ctx->get_params.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();
    };
    m.functions["post"] = [ctx](auto args) -> Value {
        if (args.empty()) {
            auto arr = std::make_shared<ValueArray>();
            for (auto& [k, v] : ctx->post_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->post_params.find(args[0].to_string());
        if (it != ctx->post_params.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();   // default (env pattern)
    };
    m.functions["body"] = [ctx](auto) -> Value {
        return Value(ctx->body);
    };
    m.functions["ip"] = [ctx](auto) -> Value {
        return Value(ctx->remote_addr);
    };
    // request::scheme() / request::host() — the address the client sees. X-Forwarded-Proto/-Host only
    // from a trusted proxy (LOOK_TRUSTED_PROXY) — the same rule as request::ip; if the application reads the header itself
    // the client can choose the scheme/host. host: lower case, name[:port]; "" if invalid.
    m.functions["scheme"] = [ctx](auto) -> Value {
        return Value(std::string(ctx->is_https ? "https" : "http"));
    };
    m.functions["host"] = [ctx](auto) -> Value {
        return Value(ctx->host);
    };
    // request::header("Authorization") — read an incoming HTTP header.
    // The key is case-insensitive (headers_in is stored lowercase: Authorization,
    // X-Api-Key, User-Agent all work). Returns null if absent.
    m.functions["header"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        std::string key = args[0].to_string();
        for (auto& c : key) c = (char)std::tolower((unsigned char)c);
        if (key == "cookie") csrf_exempt_cookie_guard(ctx.get(), "request::header(\"Cookie\")");
        auto it = ctx->headers_in.find(key);
        if (it != ctx->headers_in.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();   // default (env pattern)
    };
    // request::headers() — returns all headers as an assoc array
    m.functions["headers"] = [ctx](auto) -> Value {
        auto arr = std::make_shared<ValueArray>();
        arr->push_back(Value::assoc_marker());
        // a cookie header is not given on a request exempted with csrf::exempt (csrf_exempt_cookie_guard)
        const bool no_cookie = !ctx->csrf_exempt_rule.empty();
        for (auto& [k, v] : ctx->headers_in) {
            if (no_cookie && k == "cookie") continue;
            arr->push_back(Value(k)); arr->push_back(Value(v));
        }
        return Value(arr);
    };
    m.functions["is_get"]     = [ctx](auto) -> Value { return Value(ctx->method == "GET");     };
    m.functions["is_post"]    = [ctx](auto) -> Value { return Value(ctx->method == "POST");    };
    m.functions["is_put"]     = [ctx](auto) -> Value { return Value(ctx->method == "PUT");     };
    m.functions["is_delete"]  = [ctx](auto) -> Value { return Value(ctx->method == "DELETE");  };
    m.functions["is_patch"]   = [ctx](auto) -> Value { return Value(ctx->method == "PATCH");   };
    m.functions["is_head"]    = [ctx](auto) -> Value { return Value(ctx->method == "HEAD");    };
    m.functions["is_options"] = [ctx](auto) -> Value { return Value(ctx->method == "OPTIONS"); };
    // request::json() — parse the JSON body automatically
    m.functions["json"] = [ctx](auto) -> Value {
        if (ctx->body.empty()) return Value();
        return json_decode_document(ctx->body);
    };
    // request::all() — GET + POST params combined
    m.functions["all"] = [ctx](auto) -> Value {
        auto arr = std::make_shared<ValueArray>();
        arr->push_back(Value::assoc_marker());
        for (auto& [k, v] : ctx->get_params)  { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        for (auto& [k, v] : ctx->post_params) { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        return Value(arr);
    };
    m.functions["param"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->route_params.find(args[0].to_string());
        return it != ctx->route_params.end() ? Value(it->second) : Value();
    };

    // request::file(name) or request::file(name, options_array)
    // options keys: max_size (int bytes), allow_mime (array), allow_svg (bool)
    // Default limit: 1MB
    m.functions["file"] = [ctx](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("request::file() requires field name");

        // if it is NOT multipart/form-data there is NO file → docs contract: returns null
        // (the user handles it with `if ($f == null)`). OLD BUG: it threw → while the docs
        // said "returns null", the code threw an exception; the `if($f==null)` branch that follows the documentation
        // could NEVER be reached (dogfooding #3-A, the sibling of session::has). Also when later
        // multipart is fully wired to --mode http it is the right contract too: no file → null.
        // An uploaded file exists only in a multipart body (web.cpp fully parses the environment type). It used to
        // search for a lowercase substring in content_type: with a "Multipart/Form-Data" body the fields were
        // read while request::file returned null.
        if (ctx->uploaded_files.empty())
            return Value();  // null — no file

        std::string field = args[0].to_string();
        auto it = ctx->uploaded_files.find(field);
        if (it == ctx->uploaded_files.end() || !it->second.valid)
            return Value(); // return null if no file was selected

        const UploadedFile& uf = it->second;

        // Read the options
        size_t    max_size   = 1 * 1024 * 1024; // default 1MB
        bool      allow_svg  = false;
        std::vector<std::string> allow_mime;

        if (args.size() >= 2 && args[1].type() == Value::ARRAY) {
            auto& arr = *args[1].as_array();
            bool is_assoc = !arr.empty() && arr[0].is_assoc_marker();
            if (is_assoc) {
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    std::string key = arr[i].to_string();
                    Value&      val = arr[i+1];
                    if (key == "max_size")  max_size  = (size_t)val.to_int();
                    if (key == "allow_svg") allow_svg = val.is_truthy();
                    if (key == "allow_mime" && val.type() == Value::ARRAY) {
                        for (auto& v : *val.as_array()) {
                            if (v.type() != Value::STRING || v.is_assoc_marker()) continue;
                            allow_mime.push_back(v.to_string());
                        }
                    }
                }
            }
        }

        // Size check
        if (uf.size > max_size)
            throw std::runtime_error("Uploaded file exceeds max_size limit (" +
                                     std::to_string(max_size) + " bytes)");

        // SVG check — if allow_mime contains SVG, allow_svg is required too
        if (uf.mime == "image/svg+xml" && !allow_svg)
            throw std::runtime_error("SVG upload requires allow_svg: true option");

        // MIME check — if allow_mime is specified it must be in the list
        if (!allow_mime.empty()) {
            bool found = false;
            for (auto& m : allow_mime) if (m == uf.mime) { found = true; break; }
            if (!found)
                throw std::runtime_error("File type not allowed: " + uf.mime);
        }

        // Return value — assoc array
        auto result = std::make_shared<ValueArray>();
        result->push_back(Value::assoc_marker());
        result->push_back(Value(std::string("path")));     result->push_back(Value(uf.temp_path));
        result->push_back(Value(std::string("filename"))); result->push_back(Value(uf.orig_name));
        result->push_back(Value(std::string("mime")));     result->push_back(Value(uf.mime));
        result->push_back(Value(std::string("size")));     result->push_back(Value((int64_t)uf.size));
        result->push_back(Value(std::string("sha256")));   result->push_back(Value(uf.sha256));
        return Value(result);
    };

    return m;
}

// HTTP response header CRLF injection defense: CR/LF in the header name/value
// (and other control characters) enable response splitting / Set-Cookie injection /
// open-redirect+CRLF. Cut at the first CR/LF/NUL — what follows is the injection payload.
// (It is applied again in the serialization layer too — defense-in-depth.)
static std::string sanitize_header(const std::string& s) {
    size_t cut = s.find_first_of("\r\n");
    std::string v = (cut == std::string::npos) ? s : s.substr(0, cut);
    // Also strip the remaining CR/LF/NUL (paranoia) — normally the cut already cut it.
    v.erase(std::remove_if(v.begin(), v.end(),
        [](unsigned char c){ return c == '\r' || c == '\n' || c == '\0'; }), v.end());
    return v;
}

static Module make_response_module(WebRef ctx) {
    Module m;
    m.name = "response";

    m.functions["status"] = [ctx](auto args) -> Value {
        if (!args.empty()) ctx->set_status(args[0].to_int());
        return Value(ctx->status_code);
    };
    m.functions["header"] = [ctx](auto args) -> Value {
        if (args.size() >= 2) {
            std::string lk = args[0].to_string();
            for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
            if (lk == "set-cookie") csrf_exempt_cookie_guard(ctx.get(), "response::header(\"Set-Cookie\")");
        }
        if (args.size() >= 2)
            ctx->headers_out[sanitize_header(args[0].to_string())] =
                sanitize_header(args[1].to_string());
        return Value();
    };
    m.functions["redirect"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->set_status(args.size() >= 2 ? args[1].to_int() : 302);
        ctx->headers_out["Location"] = sanitize_header(args[0].to_string());
        return Value();
    };
    // response::json/text/html — write the body to ctx->response_body (the web.h contract).
    // The return value is lost when called as an expression statement;
    // the FCGI/HTTP output layers read the body from response_body.
    m.functions["json"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "application/json; charset=utf-8";
        if (args.size() >= 2) ctx->set_status(args[1].to_int());   // response::json($d, 201)
        if (!args.empty()) {
            std::string s = json_encode(args[0]);
            ctx->response_body += s;
            return Value(s);
        }
        return Value();
    };
    // response::error(code, message) — status code + a {"ok":false,"error":message} JSON body.
    // Error return in a single line: return response::error(404, "Not found")
    // Replaces the 3-line status+json+return pattern.
    m.functions["error"] = [ctx](auto args) -> Value {
        int code = args.empty() ? 500 : args[0].to_int();
        std::string msg = args.size() >= 2 ? args[1].to_string() : "Error";
        ctx->set_status(code);
        ctx->headers_out["Content-Type"] = "application/json; charset=utf-8";
        auto obj = std::make_shared<ValueArray>();
        obj->push_back(Value::assoc_marker());
        obj->push_back(Value(std::string("ok")));   obj->push_back(Value(false));
        obj->push_back(Value(std::string("error")));  obj->push_back(Value(msg));
        std::string s = json_encode(Value(obj));
        ctx->response_body += s;
        return Value(s);
    };
    m.functions["text"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "text/plain; charset=utf-8";
        if (!args.empty()) ctx->response_body += args[0].to_string();
        return Value();
    };
    m.functions["html"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "text/html; charset=utf-8";
        if (!args.empty()) ctx->response_body += args[0].to_string();
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
        // Memory budget: decoded values are larger than the text (32 bytes per value + texts) — roughly
        // 4 times is asked in advance (look/mem_budget.h); so that a huge document does not finish before reaching a safe point.
        if (args[0].type() == Value::STRING) look::mem_reserve((int64_t)args[0].str_ref().size() * 4, "json::decode()");
        return json_decode_document(args[0].to_string());
    };

    return m;
}

// Percent-encoding for a cookie name/value.
//
// OLD BUG: sanitize_header() cleaned only CR/LF/NUL — correct for general headers,
// but in a cookie ';' is a STRUCTURAL SEPARATOR. When a user-controlled value
// was written to a cookie (a very common pattern like a language/theme preference) an attacker could control the COOKIE
// ATTRIBUTES:
//   ?v=x; HttpOnly; Domain=evil.com
//   -> Set-Cookie: preference=x; HttpOnly; Domain=evil.com; Path=/
// Effect: pulling Domain up to the parent domain to leak the cookie to ALL subdomains,
// making it persistent with Max-Age, changing the application's intent with Path/Secure.
// (CRLF injection was already blocked — this is a different channel from that.)
//
// The fix is the same as PHP setcookie(): encode when writing, decode when reading. Legitimate data
// (a value containing ';') is preserved on a round trip but cannot carry structural meaning.
static std::string cookie_enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) {
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0xF]; }
    }
    return o;
}
static std::string cookie_dec(const std::string& s) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexval(s[i+1]), l = hexval(s[i+2]);
            if (h >= 0 && l >= 0) { o += (char)((h << 4) | l); i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

static Module make_cookie_module(WebRef ctx) {
    Module m;
    m.name = "cookie";

    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->cookies_in.find(args[0].to_string());
        if (it != ctx->cookies_in.end()) return Value(cookie_dec(it->second));
        return args.size() >= 2 ? args[1] : Value();   // default (env pattern)
    };
    m.functions["set"] = [ctx](auto args) -> Value {
        if (args.size() < 2) return Value();
        // Percent-encode: so that ';' and other structural characters cannot inject an attribute.
        std::string name    = cookie_enc(args[0].to_string());
        std::string value   = cookie_enc(args[1].to_string());

        // SECURE-BY-DEFAULT (2026-08 flip): HttpOnly + SameSite=Lax are the DEFAULT — aligned with session:: (it
        // already took those three hardcoded; cookie:: took none of them → an inconsistency,
        // a user who learned "LOOK cookies are secure" was mistaken with cookie::set). Secure is NOT
        // the default (it would silently break plain-HTTP dev). Opt-out: the options assoc ["httponly"=>false].
        int expires = 0;
        std::string path_c = "/";
        bool httponly = true, secure = false;
        std::string samesite = "Lax";
        // The remaining arguments are TYPE-BASED: ARRAY=options assoc · int=expires · string=path.
        // So set(k,v) · set(k,v,3600) · set(k,v,3600,"/") · set(k,v,["httponly"=>false]) all work.
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i].type() == Value::ARRAY) {
                auto& a = *args[i].as_array();
                size_t s = (!a.empty() && a[0].is_assoc_marker()) ? 1 : 0;
                for (size_t j = s; j + 1 < a.size(); j += 2) {
                    std::string k = a[j].to_string();
                    if      (k == "httponly") httponly = a[j+1].is_truthy();
                    else if (k == "secure")   secure   = a[j+1].is_truthy();
                    else if (k == "samesite") samesite = a[j+1].to_string(); // "" / "false" → skip
                }
            } else if (args[i].type() == Value::INT) {
                expires = (int)args[i].to_int();
            } else if (args[i].type() == Value::STRING) {
                path_c = args[i].to_string();
            }
        }
        // Path and SameSite go into Set-Cookie as attributes: ';' would open a new attribute
        // (from the path "; Domain=evil.example; Max-Age=…" — if user input becomes the path). RFC 6265
        // path-av: no control characters and no ';'. An explicit error, not a silent cleanup.
        for (unsigned char c : path_c)
            if (c == ';' || c < 0x20 || c == 0x7f)
                throw std::runtime_error("cookie::set() path must not contain ';' or control characters");
        {
            std::string s = samesite;
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            if (s == "strict") samesite = "Strict";
            else if (s == "lax") samesite = "Lax";
            else if (s == "none") samesite = "None";
            else if (!(s.empty() || s == "false" || s == "0"))
                throw std::runtime_error("cookie::set() samesite must be \"Strict\", \"Lax\" or \"None\"");
        }

        std::string cookie = name + "=" + value + "; Path=" + path_c;
        if (expires > 0) {
            time_t exp = std::time(nullptr) + expires;
            char buf[64];
            // gmtime returns a shared static buffer (thread race) — gmtime_r
            struct tm tm_buf{};
#ifdef _WIN32
            gmtime_s(&tm_buf, &exp);
#else
            gmtime_r(&exp, &tm_buf);
#endif
            strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
            cookie += "; Expires=" + std::string(buf);
        }
        // Security attributes (default HttpOnly+SameSite=Lax; turned off with opt-out).
        if (httponly) cookie += "; HttpOnly";
        if (secure)   cookie += "; Secure";
        if (!samesite.empty() && samesite != "false" && samesite != "0")
            cookie += "; SameSite=" + samesite;
        // Each cookie is a separate Set-Cookie line — std::map would overwrite the single key.
        ctx->set_cookies_out.push_back(cookie);
        return Value();
    };
    m.functions["delete"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->set_cookies_out.push_back(
            cookie_enc(args[0].to_string()) + "=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
        return Value();
    };
    m.functions["has"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value(false);
        return Value(ctx->cookies_in.count(args[0].to_string()) > 0);
    };

    // No cookie on a request exempted with csrf::exempt (same rule as session:: — make_session_module).
    // OLD: session:: gave an error but cookie:: was open → an exempt route could authenticate with a cookie
    // (a CSRF hole); WRITING a cookie can also be triggered from another site (login CSRF: getting the victim into the attacker's
    // account), DELETING logs the victim out of the session. An exempt route never touches the cookie. The exemption works only on
    // unsafe methods: on GET and on a non-exempt route cookies are as usual.
    for (auto& [name, fn] : m.functions) {
        auto inner = fn;
        fn = [ctx, inner, name = name](std::vector<Value> args) -> Value {
            csrf_exempt_cookie_guard(ctx, ("cookie::" + name + "()").c_str());
            return inner(args);
        };
    }
    return m;
}

// On a request exempted with csrf::exempt the cookie is NEITHER read NOR written by ANY path — cookie::, request::header
// ("Cookie"), response::header("Set-Cookie"), env("HTTP_COOKIE") in CGI. OLD: only cookie:: was closed;
// the same cookie could be read from the header (measured: Cookie: sid=VICTIM → response sid=VICTIM). request::headers()
// never gives the cookie header on an exempt request. The message is in one place.
void csrf_exempt_cookie_guard(const WebContext* w, const char* what) {
    if (w && !w->csrf_exempt_rule.empty())
        throw std::runtime_error(std::string(what) + " in a request exempted from the CSRF check by csrf::exempt(\"" +
            w->csrf_exempt_rule + "\"): an exempt route cannot use cookies. If the route needs to know the user, "
            "do not exempt it (send the CSRF token); a webhook verifies its signature instead");
}

// ── Session storage backend ───────────────────────────────────────────────────
// LOOK_SESSION_DRIVER=redis + LOOK_REDIS_URL → Redis (multi-server session —
// sessions are shared behind a load balancer). Otherwise a file (single server).
// The blob format is the same in both backends: "key=value\n" lines.
static bool session_use_redis() {
    static const bool v = []() {
        const char* d = std::getenv("LOOK_SESSION_DRIVER");
        const char* u = std::getenv("LOOK_REDIS_URL");
        return d && std::string(d) == "redis" && u && *u;
    }();
    return v;
}
static int session_ttl() {
    static const int v = []() {
        const char* t = std::getenv("LOOK_SESSION_TTL");
        int n = t ? std::atoi(t) : 0;
        return n > 0 ? n : 86400;   // default 1 day
    }();
    return v;
}
// Redis session connections come from a pool: a request takes a connection and gives it back as soon as the command finishes;
// if there is no idle connection a new one is opened (lazy). It used to be ONE connection per thread:
// while a fiber waited on Redis another request on the same thread could write a command to the same connection
// and read the other's reply — user A's session went to B
// (tests/redis_session_isolation_test.sh). On an error or a timeout RespClient closes the connection;
// a closed connection does not return to the pool, the next request opens a new one. The error reaches the request:
// it used to be swallowed (when Redis did not answer the session was silently empty, written data silently lost).
static void with_session_redis(const std::function<void(RespClient&)>& op) {
    static std::mutex mu;
    static std::vector<std::unique_ptr<RespClient>> idle;
    std::unique_ptr<RespClient> c;
    for (;;) {
        {
            std::lock_guard<std::mutex> g(mu);
            if (idle.empty()) break;
            c = std::move(idle.back()); idle.pop_back();
        }
        // If Redis restarted every connection in the pool is dead: it is noticed before sending,
        // dropped, the next one is tried (or a new one is opened if none). No command is repeated.
        if (c->usable_when_idle()) break;
        c.reset();
    }
    try {
        if (!c) {   // outside the lock: the fiber may be suspended while connecting
            const char* u = std::getenv("LOOK_REDIS_URL");
            c = std::make_unique<RespClient>(u ? std::string(u) : std::string("redis://127.0.0.1:6379"));
        }
        op(*c);
    } catch (const std::exception& e) {
        // If not caught: 503 + a log (not an application error, an outage of the session store).
        throw look::ServiceUnavailable(std::string("session: ") + e.what());
    }
    if (c->is_connected()) {
        std::lock_guard<std::mutex> g(mu);
        if (idle.size() < 256) idle.push_back(std::move(c));
    }
}
// SECURITY: Session ID validation — the generated format is EXACTLY 32 hex characters. The sid
// comes from the client's LOOK_SESSION cookie and goes directly into the file path
// (the session file path). If not validated, a value like "x/../../../etc/cron.d/pwn" gives
// path traversal → arbitrary file read (session::get) and write/truncate
// (session::set → ofstream trunc). Strict allowlist: only 32 hex accepted.
using look::valid_sid;
// The session blob is in a line-based "key=value\n" format. Keys/values were NOT
// ESCAPED AT ALL: a \n inside a value defined A NEW FIELD.
//
// MEASURED ATTACK (if the application writes user data into the session — very common):
//   session::set("role", "member")                // privilege first
//   session::set("name", request::get("name"))    // then user data
//   ?name=bob\nrole=admin\nadmin=1
//   -> blob: role=member / name=bob / role=admin / admin=1
//   -> session::get("admin") = "1"   A PRIVILEGE FIELD THAT DID NOT EXIST AT ALL WAS INJECTED
// The attacker could forge fields like admin/user_id/is_admin in a request before login. A '=' in the key also confused the data:
//   set("a=b","X") -> get("a") = "b=X"
//   set("a=b","X") -> get("a") = "b=X"
//
// The fix: escape when writing/reading. A line ending in a value can be LEGITIMATE user data
// (a textarea), so escaping, not rejecting, is right — the data is
// preserved on a round trip, the separator carries no meaning.
// Backward compatible: old blobs have no backslash, unescape acts as the identity function.
// sess_esc/unesc/blob_get/blob_set → look/session_esc.h (pure, table-testable).
using look::sess_esc;
using look::sess_unesc;
using look::sess_blob_get;
using look::sess_blob_set;
static std::string sess_load(const std::string& sid) {
    if (session_use_redis()) {
        std::string blob;
        with_session_redis([&](RespClient& r) { blob = r.get("look_sess:" + sid); });
        return blob;
    }
    bool foreign = false;
    std::string blob = look::session_file_read(sid, &foreign, session_ttl());   // empty if expired
    if (foreign)   // a file this process did not write: never a session
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "SESSION",
            "ignored a session file not written by this process (wrong owner, permissions or a link): " + look::session_file(sid));
    return blob;
}
// The maximum size of a session (bytes). LOOK_SESSION_MAX_BYTES; default 1 MB.
static size_t session_max_bytes() {
    static const size_t v = [] {
        const char* e = std::getenv("LOOK_SESSION_MAX_BYTES");
        if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
        return (size_t)1 << 20;
    }();
    return v;
}
static void sess_store(const std::string& sid, const std::string& blob) {
    if (session_use_redis()) {
        with_session_redis([&](RespClient& r) { r.set("look_sess:" + sid, blob, session_ttl()); });
        return;
    }
    look::session_file_write(sid, blob);   // 0600, links are not followed
    look::session_files_gc(session_ttl());  // delete the expired ones in the background
}
// Update an existing session; false = the session was deleted in the meantime (see session_file_update).
static bool sess_update(const std::string& sid, const std::string& blob) {
    if (session_use_redis()) {
        bool ok = false;
        with_session_redis([&](RespClient& r) { ok = r.set_if_exists("look_sess:" + sid, blob, session_ttl()); });
        return ok;
    }
    bool ok = look::session_file_update(sid, blob);
    look::session_files_gc(session_ttl());
    return ok;
}
static bool sess_exists(const std::string& sid) {
    if (session_use_redis()) {
        bool e = false;
        with_session_redis([&](RespClient& r) { e = r.exists("look_sess:" + sid); });
        return e;
    }
    return look::session_file_exists(sid, session_ttl());
}
static void sess_remove(const std::string& sid) {
    if (session_use_redis()) {
        with_session_redis([&](RespClient& r) { r.del("look_sess:" + sid); });
        return;
    }
    look::session_file_remove(sid);
}
// Generate a 32-hex CSPRNG session ID (fail-closed) — shared by session::start and regenerate.
// /dev/urandom (Linux) or rand_s (Windows). If there is no secure source
// throw an error instead of emitting a weak ID.
static std::string gen_session_id() {
    char id[33];
#if defined(_WIN32)
    for (int i = 0; i < 32; i++) { unsigned int r = 0; rand_s(&r); id[i] = "0123456789abcdef"[r % 16]; }
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    uint8_t bytes[16];
    urandom.read(reinterpret_cast<char*>(bytes), 16);
    if (!urandom || urandom.gcount() != 16)
        throw std::runtime_error("session: could not obtain secure randomness (/dev/urandom)");
    for (int i = 0; i < 16; i++) {
        id[i*2]   = "0123456789abcdef"[(bytes[i] >> 4) & 0xf];
        id[i*2+1] = "0123456789abcdef"[bytes[i] & 0xf];
    }
#endif
    id[32] = 0;
    return std::string(id);
}

// Decide the session cookie's Secure attribute. LOOK_SESSION_SECURE forces it: "1" = always,
// "0" = never (dev). Unset = conditional on a real HTTPS signal (ctx->is_https). The risk this
// guards: behind a TLS proxy that forwards over plain HTTP, if LOOK_TRUSTED_PROXY is not set the
// proxy's X-Forwarded-Proto is (correctly) distrusted, so is_https is false and Secure would be
// silently dropped in production. So when we ship a session cookie without Secure over a non-HTTPS
// request and nothing was set explicitly, we WARN once — behaviour unchanged, but never silent.
static std::string session_secure_attr(WebContext* ctx) {
    static const int mode = []{
        const char* e = std::getenv("LOOK_SESSION_SECURE");
        return !e ? -1 : (std::string(e) == "0" ? 0 : 1);
    }();
    if (mode == 1) return "; Secure";
    if (mode == 0) return "";
    if (ctx && ctx->is_https) return "; Secure";
    static std::atomic<bool> warned{false};
    bool expected = false;
    if (warned.compare_exchange_strong(expected, true))
        Logger::instance().log(LogLevel::LOG_WARN, "SESSION",
            "session cookie sent WITHOUT Secure (no HTTPS signal on this request). If LOOK runs "
            "behind a TLS-terminating proxy, set LOOK_TRUSTED_PROXY so X-Forwarded-Proto=https is "
            "honoured; or set LOOK_SESSION_SECURE=1 to force it (LOOK_SESSION_SECURE=0 silences this "
            "for plain-HTTP dev). Otherwise the session cookie can be sent over http and hijacked.");
    return "";
}

// ── csrf:: — no state on the server: a signed double-submit cookie ──────────────
// (gorilla/csrf, OWASP "signed double-submit"; the same algorithm as LookPress src/security.lk)
//   cookie look_csrf = base . "." . HMAC(LOOK_SECRET, "csrf|" . base)   on HTTPS __Host-look_csrf (Secure)
//   form  _csrf     = nonce . "." . HMAC(base, nonce)   — different on every call (BREACH)
// Writing the key to the session made every visitor who saw a form open a session file on the server
// (cookieless requests could fill the disk). Verification: the cookie signature is valid with LOOK_SECRET or
// LOOK_SECRET_PREVIOUS, the MAC of the submitted value matches the base, and Origin, if present, is this site.
// session::regenerate/destroy (login/logout) gives a new base: a key captured or
// imposed before login does not pass afterwards. __Host- prefix: a cookie cannot be imposed from a subdomain.
static std::string csrf_hex(const std::vector<uint8_t>& b) {
    static const char* hx = "0123456789abcdef";
    std::string o; o.reserve(b.size() * 2);
    for (uint8_t c : b) { o += hx[c >> 4]; o += hx[c & 0xF]; }
    return o;
}
static std::string csrf_mac(const std::string& key, const std::string& msg) {
    return csrf_hex(look::hmac_sha256(std::vector<uint8_t>(key.begin(), key.end()),
                                      std::vector<uint8_t>(msg.begin(), msg.end())));
}
static bool csrf_equal(const std::string& a, const std::string& b) {   // constant-time
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}
// Signing keys: the current one first, then (if any) the previous one — so open forms do not get 403 while the key changes.
// require: throw if there is no key (csrf:: calls) or return empty (refresh from inside session::).
static std::vector<std::string> csrf_secrets(bool require) {
    const char* cur = std::getenv("LOOK_SECRET");
    std::string c = cur ? cur : "";
    if (c.size() < 32) {
        if (!require) return {};
        throw std::runtime_error("csrf: LOOK_SECRET is not set or shorter than 32 characters. Set it to a "
            "random value, the same on every server (e.g. openssl rand -hex 32): CSRF tokens are signed with it");
    }
    std::vector<std::string> v{c};
    // The previous key obeys the same rule (≥ 32): a short previous key ("x") could be guessed and a valid
    // CSRF cookie signed — the length rule of the current key was bypassed this way. A short one
    // is never used; the startup check (csrf_secret_problem) stops the application with the reason.
    const char* p = std::getenv("LOOK_SECRET_PREVIOUS");
    if (p && std::strlen(p) >= 32) v.push_back(p);
    return v;
}
// Secure: the same decision as the session cookie (LOOK_SESSION_SECURE 1/0 forces; otherwise real HTTPS).
static bool csrf_secure(WebContext* ctx) {
    const char* e = std::getenv("LOOK_SESSION_SECURE");
    if (e) return std::string(e) != "0";
    return ctx && ctx->is_https;
}
static std::string csrf_cookie_name(WebContext* ctx) {
    return csrf_secure(ctx) ? "__Host-look_csrf" : "look_csrf";
}
// The base in the cookie: the base if its signature is valid with one of the keys (0..max_key), otherwise "".
static std::string csrf_cookie_base(WebContext* ctx, size_t max_key) {
    auto it = ctx->cookies_in.find(csrf_cookie_name(ctx));
    if (it == ctx->cookies_in.end()) return "";
    const std::string& v = it->second;
    auto dot = v.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == v.size() || v.find('.', dot + 1) != std::string::npos)
        return "";
    std::string base = v.substr(0, dot), sig = v.substr(dot + 1);
    auto keys = csrf_secrets(true);
    for (size_t i = 0; i < keys.size() && i <= max_key; ++i)
        if (csrf_equal(sig, csrf_mac(keys[i], "csrf|" + base))) return base;
    return "";
}
static void csrf_set_cookie(WebContext* ctx, const std::string& base) {
    std::string name = csrf_cookie_name(ctx);
    auto& out = ctx->set_cookies_out;   // second time in the same request (refresh): the old line goes
    out.erase(std::remove_if(out.begin(), out.end(),
        [&](const std::string& s) { return s.rfind(name + "=", 0) == 0; }), out.end());
    out.push_back(name + "=" + base + "." + csrf_mac(csrf_secrets(true)[0], "csrf|" + base) +
                  "; Path=/; HttpOnly" + (csrf_secure(ctx) ? "; Secure" : "") + "; SameSite=Lax");
    ctx->csrf_base = base;
}
// This request's base: the current cookie if valid (if signed with the previous key it is re-signed
// with the current key), otherwise a new base + cookie. Always the same base within a request.
static std::string csrf_base(WebContext* ctx) {
    if (!ctx->csrf_base.empty()) return ctx->csrf_base;
    std::string b = csrf_cookie_base(ctx, 0);
    if (!b.empty()) { ctx->csrf_base = b; return b; }
    b = csrf_cookie_base(ctx, 1);
    if (b.empty()) b = csrf_hex(look::secure_random_bytes(32));
    csrf_set_cookie(ctx, b);
    return b;
}
// session::regenerate / session::destroy: a new base if the application uses csrf:: (in this request or if its cookie
// came). Does nothing in an application that does not use it (does not ask for LOOK_SECRET either).
static void csrf_rotate_if_used(WebContext* ctx) {
    bool used = !ctx->csrf_base.empty() || ctx->cookies_in.count("look_csrf") ||
                ctx->cookies_in.count("__Host-look_csrf");
    if (!used || csrf_secrets(false).empty()) return;
    csrf_set_cookie(ctx, csrf_hex(look::secure_random_bytes(32)));
}
// If there is an Origin header, is it from this site (request::host — X-Forwarded-Host only from a trusted proxy).
static bool csrf_origin_ok(WebContext* ctx) {
    auto it = ctx->headers_in.find("origin");
    if (it == ctx->headers_in.end() || it->second.empty()) return true;   // old browser: key only
    std::string o = it->second;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    if (o == "null") return false;                                           // opaque origin (sandbox, data:)
    auto p = o.find("://");
    if (p == std::string::npos) return false;
    std::string oh = o.substr(p + 3);
    if (!ctx->host.empty() && oh == ctx->host) return true;
    auto h = ctx->headers_in.find("host");
    return h != ctx->headers_in.end() && !oh.empty() && oh == WebContext::clean_host(h->second);
}

static Module make_csrf_module(WebRef ctx) {
    Module m;
    m.name = "csrf";
    // csrf::token() — the value put in a form or the X-CSRF-Token header; a new nonce on every call.
    m.functions["token"] = [ctx](auto) -> Value {
        std::string base = csrf_base(ctx);
        std::string nonce = csrf_hex(look::secure_random_bytes(16));
        return Value(nonce + "." + csrf_mac(base, nonce));
    };
    // csrf::field() — a hidden field put inside a form (the value is only [0-9a-f.] — no escaping needed).
    m.functions["field"] = [ctx](auto) -> Value {
        std::string base = csrf_base(ctx);
        std::string nonce = csrf_hex(look::secure_random_bytes(16));
        return Value("<input type=\"hidden\" name=\"_csrf\" value=\"" + nonce + "." + csrf_mac(base, nonce) + "\">");
    };
    // csrf::exempt(path) — only at top level (http_main's setup table registers it); if called here
    // it is during a request: an explicit error.
    m.functions["exempt"] = [](auto) -> Value {
        throw std::runtime_error("csrf::exempt() is called at the top level of the program, next to route(), not inside a route");
    };
    return m;
}

// ── Default CSRF gate (http_main: three modes from the same engine) ──────────────
// Rule: every application that uses cookies is protected — if the program has session::/csrf::/cookie::
// every request other than GET/HEAD/OPTIONS is checked HERE, BEFORE the route and before_route.
std::string csrf_secret_problem() {
    const char* cur = std::getenv("LOOK_SECRET");
    const char* prev = std::getenv("LOOK_SECRET_PREVIOUS");
    if (cur && std::string(cur).size() >= 32) {
        if (prev && *prev && std::strlen(prev) < 32)
            return "LOOK_SECRET_PREVIOUS is shorter than 32 characters. It still verifies tokens signed before the "
                   "key changed, so a short value would let anyone who guesses it sign their own; set it to the "
                   "previous LOOK_SECRET (32+ characters) or remove it";
        return "";
    }
    return "LOOK_SECRET is not set or shorter than 32 characters. This application uses cookies, and every "
           "application that uses cookies is protected against cross-site request forgery: every request other than GET, HEAD and "
           "OPTIONS carries a token signed with LOOK_SECRET. Set it to a random value, the same on every server "
           "(openssl rand -hex 32) — in the service's environment file, or with Docker: "
           "docker run -e LOOK_SECRET=<value> … or --env-file <file>";
}
bool csrf_request_ok(WebContext* ctx, std::string& why) {
    // Sec-Fetch-Site (the browser writes it itself, the page cannot change it): a request from another site or a sibling subdomain
    // is rejected (Go 1.25 CrossOriginProtection). "same-origin" and "none"
    // (address bar, bookmark) pass; if the header is absent (old browser, server to server) Origin + token.
    {
        auto sf = ctx->headers_in.find("sec-fetch-site");
        if (sf != ctx->headers_in.end()) {
            std::string v = sf->second;
            for (auto& c : v) c = (char)std::tolower((unsigned char)c);
            if (v == "cross-site" || v == "same-site") {
                why = "Sec-Fetch-Site: " + v + " (the request came from another site)";
                return false;
            }
        }
    }
    if (!csrf_origin_ok(ctx)) {
        auto o = ctx->headers_in.find("origin");
        why = "Origin " + (o == ctx->headers_in.end() ? std::string("?") : o->second) + " is not this site" +
              (ctx->host.empty() ? std::string() : " (" + ctx->host + ")");
        return false;
    }
    std::string sent;
    auto pp = ctx->post_params.find("_csrf");
    if (pp != ctx->post_params.end()) sent = pp->second;
    if (sent.empty()) { auto h = ctx->headers_in.find("x-csrf-token"); if (h != ctx->headers_in.end()) sent = h->second; }
    if (sent.empty()) { why = "no _csrf field or X-CSRF-Token header"; return false; }
    std::string base = csrf_cookie_base(ctx, 1);
    if (base.empty()) { why = "no valid CSRF cookie (the form was not loaded from this site, cookies are blocked, or LOOK_SECRET changed)"; return false; }
    auto dot = sent.find('.');
    if (dot == std::string::npos || dot == 0 || sent.find('.', dot + 1) != std::string::npos ||
        !csrf_equal(sent.substr(dot + 1), csrf_mac(base, sent.substr(0, dot)))) {
        why = "the token does not belong to this visitor's CSRF cookie";
        return false;
    }
    return true;
}

// This request's session id — only an id this server ISSUED and that has not expired (strict mode).
// If there is no cookie, its format is broken, or there is no such session in the store, a new CSPRNG id is issued,
// an empty record is written to the store and the cookie is sent. It used to check only the format: if an attacker
// planted "LOOK_SESSION=<32 hex>" in the victim's browser (a sibling subdomain, plain HTTP) then when the victim
// logged in the attacker used their session with the same id (session fixation).
// It is decided once per request: since the new id is written to cookies_in, the second call finds it.
// The short-lived "moved" marker (120 s) that regenerate leaves on the old id. destroy leaves none:
// a concurrent regenerate carries the copy from the start of the request if the marker exists, and does not (logout) if not.
static constexpr int SESS_MOVED_TTL = 120;
static void sess_mark_moved(const std::string& sid) {
    if (session_use_redis()) {
        with_session_redis([&](RespClient& r) { r.set("look_sess_moved:" + sid, "1", SESS_MOVED_TTL); });
        return;
    }
    look::session_file_write("moved_" + sid, "");
}
static bool sess_was_moved(const std::string& sid) {
    if (session_use_redis()) {
        bool e = false;
        with_session_redis([&](RespClient& r) { e = r.exists("look_sess_moved:" + sid); });
        return e;
    }
    return look::session_file_exists("moved_" + sid, SESS_MOVED_TTL);
}

// Existence + content, in one round (Redis GET / a single file open) — the same cost as sess_exists.
static bool sess_load_if_exists(const std::string& sid, std::string& blob) {
    if (session_use_redis()) {
        bool e = false;
        with_session_redis([&](RespClient& r) { e = r.get_if_exists("look_sess:" + sid, blob); });
        return e;
    }
    return look::session_file_read_if_exists(sid, blob, session_ttl());
}

static std::string session_issued_sid(WebContext* ctx) {
    auto it = ctx->cookies_in.find("LOOK_SESSION");
    if (it != ctx->cookies_in.end() && valid_sid(it->second)) {
        // The data at the start of the request is kept: so that if another request regenerates at the same time and deletes the old record, this
        // request's regenerate does not lose the data (measured: of 20 concurrent regenerates 19 gave
        // an empty session → as if the user had logged out). A request that starts AFTER the record is deleted (including the old id
        // the attacker fixed) gets nothing: the copy is filled only for an existing id.
        if (ctx->sess_snap_sid == it->second) return it->second;
        std::string blob;
        if (sess_load_if_exists(it->second, blob)) {
            ctx->sess_snap_sid = it->second;
            ctx->sess_snap = std::move(blob);
            return it->second;
        }
    }
    std::string sid = gen_session_id();
    sess_store(sid, "");
    ctx->cookies_in["LOOK_SESSION"] = sid;
    ctx->set_cookies_out.push_back("LOOK_SESSION=" + sid + "; Path=/; HttpOnly" + session_secure_attr(ctx) + "; SameSite=Lax");
    return sid;
}

static Module make_session_module(WebRef ctx) {
    Module m;
    m.name = "session";

    m.functions["start"] = [ctx](auto) -> Value {
        // Never trust the id in the cookie: only an id this server issued (session_issued_sid).
        return Value(session_issued_sid(ctx));
    };
    // session::regenerate() — session fixation protection. Called on login / a
    // privilege change: moves to a new SID WHILE KEEPING the current session data, deletes the old one,
    // sets a new cookie. A SID the attacker fixed in advance
    // becomes invalid after login. OWASP: renew the SID on a privilege change.
    m.functions["regenerate"] = [ctx](auto) -> Value {
        std::string blob;
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it != ctx->cookies_in.end() && valid_sid(it->second)) {
            const std::string old = it->second;
            bool found = sess_load_if_exists(old, blob);   // take the current data (including what was written in the meantime)
            if (found) {
                sess_mark_moved(old);       // marker first, then delete: a concurrent regenerate sees the marker
                sess_remove(old);           // invalidate the old session
            } else if (ctx->sess_snap_sid == old && sess_was_moved(old)) {
                // A concurrent regenerate moved the old record after this request started → the copy
                // from the start of the request is carried. If destroy (logout) deleted it there is no marker → the copy is NOT USED:
                // a logout is not undone. The copy is filled only for an id that existed at the start of the request.
                blob = ctx->sess_snap;
            }
        }
        ctx->sess_snap_sid.clear(); ctx->sess_snap.clear();
        std::string sid = gen_session_id();
        ctx->cookies_in["LOOK_SESSION"] = sid;
        ctx->set_cookies_out.push_back("LOOK_SESSION=" + sid + "; Path=/; HttpOnly" + session_secure_attr(ctx) + "; SameSite=Lax");
        sess_store(sid, blob);   // move the data to the new SID (even if empty: so it counts as "issued" in strict mode)
        csrf_rotate_if_used(ctx);                   // login: a new CSRF base (if csrf:: is used)
        return Value(sid);
    };
    m.functions["set"] = [ctx](auto args) -> Value {
        if (args.size() < 2) return Value();
        // Session values are stored as strings. Fail loud on a structure instead of
        // silently coercing it (an array/assoc would to_string() to "[__assoc__, …]"
        // and lose its shape on read) — the caller must JSON-encode it themselves.
        switch (args[1].type()) {
            case Value::STRING: case Value::INT: case Value::FLOAT: case Value::BOOL: case Value::NONE:
                break;
            case Value::ARRAY:
                throw std::runtime_error("session::set() — session values are strings; JSON-encode a structure first, e.g. session::set(key, json::encode(value)) and json::decode(session::get(key))");
            default:
                throw std::runtime_error("session::set() — session values must be a string or number");
        }
        // The write goes to an issued session: if the id in the cookie is invalid or an id this server did not issue,
        // a new id is issued (it used to be that set, even without start, wrote to any
        // 32-hex id in the cookie and made it "exist" — fixation worked through this path too).
        const std::string sid = session_issued_sid(ctx);
        std::string blob = sess_load(sid);
        sess_blob_set(blob, args[0].to_string(), args[1].to_string());
        // The upper limit of a session (the same in the Redis and file drivers): it did not exist before — a visitor
        // could grow their own session by megabytes and make it be read that much on every request, and with a few
        // visitors fill Redis memory. When exceeded the data is not written, and not silently
        // truncated either: an explicit error. The previous session stays as it was.
        if (blob.size() > session_max_bytes())
            throw std::runtime_error("session::set() — the session would be " + std::to_string(blob.size()) +
                " bytes, more than the limit of " + std::to_string(session_max_bytes()) +
                " (LOOK_SESSION_MAX_BYTES); keep large data in the database and store its id in the session");
        if (!sess_update(sid, blob)) {
            // The session was deleted while this request was running (logout in another tab / regenerate): the old data is NOT
            // WRITTEN back — it used to be written and the logout was undone. Only this key
            // is written to a new, empty session.
            ctx->cookies_in.erase("LOOK_SESSION");
            const std::string nsid = session_issued_sid(ctx);
            std::string fresh;
            sess_blob_set(fresh, args[0].to_string(), args[1].to_string());
            sess_update(nsid, fresh);
        }
        return Value();
    };
    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it == ctx->cookies_in.end() || !valid_sid(it->second)) return Value();  // traversal guard
        std::string blob = sess_load(it->second), out;
        if (sess_blob_get(blob, args[0].to_string(), out)) return Value(out);
        return args.size() >= 2 ? args[1] : Value();   // default (env pattern)
    };
    // session::has(key) — does the key exist in the session? (documented in the docs; separate from the get default:
    // a key whose value is an empty string also counts as PRESENT)
    m.functions["has"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value(false);
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it == ctx->cookies_in.end() || !valid_sid(it->second)) return Value(false);
        std::string blob = sess_load(it->second), out;
        return Value(sess_blob_get(blob, args[0].to_string(), out));
    };
    m.functions["destroy"] = [ctx](auto) -> Value {
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it != ctx->cookies_in.end() && valid_sid(it->second)) sess_remove(it->second);
        ctx->cookies_in.erase("LOOK_SESSION");  // so the next session::start() generates a new ID
        ctx->sess_snap_sid.clear(); ctx->sess_snap.clear();   // the copy is not carried after logout
        ctx->set_cookies_out.push_back(
            std::string("LOOK_SESSION=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly") + session_secure_attr(ctx) + "; SameSite=Lax");
        csrf_rotate_if_used(ctx);                   // logout: a new CSRF base
        return Value();
    };

    // NO session on a request exempted with csrf::exempt: the exemption is only for endpoints that do not rely on a cookie
    // (signed webhook, token API). "Exempt but verify with the session" is a CSRF hole — it must not be possible to write it.
    for (auto& [name, fn] : m.functions) {
        auto inner = fn;
        fn = [ctx, inner, name = name](std::vector<Value> args) -> Value {
            if (ctx && !ctx->csrf_exempt_rule.empty())
                throw std::runtime_error("session::" + name + "() in a request exempted from the CSRF check by csrf::exempt(\"" +
                    ctx->csrf_exempt_rule + "\"). An exempt route is for webhooks and token APIs, which do not use the "
                    "session; a route that uses the session must stay protected (remove the exemption and send a token)");
            return inner(args);
        };
    }
    return m;
}

// â”€â”€ Route module â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// route() is a built-in function (compiler + web dispatch), not part of this module.
// This module provides route::params() helper.

static Module make_route_module(WebRef ctx) {
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
    // The authority section ends at the first '/' or '?' (Go net/url). Without a path "h:3306?tls=1" used to
    // put the query into the port text → ?tls silently vanished. db carries the query ("?tls=1").
    size_t slash = rest.find_first_of("/?");
    std::string hostport = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    db = (slash == std::string::npos) ? "" : rest[slash] == '/' ? rest.substr(slash + 1) : rest.substr(slash);
    // host[:port] — Go net/url model (look/host_port.h): digits only, 1-65535, an empty port =
    // the default, IPv6 in square brackets. It used to be stoi: "3306x" → 3306, "abc" → a bare "stoi".
    // The error text does NOT CONTAIN the whole DSN (so the password does not leak) — only the host:port part.
    std::string err = look::split_host_port(hostport, host, port);
    if (!err.empty()) throw std::runtime_error("db::connect(): invalid DSN: " + err);

    return scheme;
}

// ── Connection pool — thread-safe, per-request borrow/return ──────────────────
//
// Fiber-aware acquire: when the pool is exhausted and we're running inside a
// fiber, the fiber yields cooperatively (suspend_current) and is re-queued on
// ITS OWN scheduler by notify_fiber_waiters() when a slot becomes free.
//
// Cross-scheduler safety: with SO_REUSEPORT / per-worker FiberSchedulers, each
// worker has a separate scheduler.  The old per-scheduler pool_waiters_ list
// meant Worker-2's release() could never wake a fiber waiting on Worker-1's
// scheduler.  The global g_fiber_waiters vector stores {fiber, scheduler, pool}
// so release() always calls resume_fiber() on the correct scheduler.

struct ConnPool {
    std::string                                dsn;
    std::vector<std::shared_ptr<DbConnection>> all;
    std::vector<std::shared_ptr<DbConnection>> available;
    std::mutex                                 mtx;
    std::condition_variable                    cv;
    // Lazy pool (like Go database/sql): db::connect opens one connection; the rest as requests
    // need them, up to max_size. It used to open the whole pool (as many as the worker count) up front in db::connect.
    // opening = the number of connections currently being opened (outside the lock).
    int                                        max_size = 1;
    int                                        opening  = 0;
    std::function<std::shared_ptr<DbConnection>()> opener;

    std::shared_ptr<DbConnection> acquire() {
        look::Fiber*          fiber = look::Fiber::current();
        look::FiberScheduler* sched = look::get_thread_scheduler();
        std::shared_ptr<look::Fiber> shared_fiber;
        if (fiber && sched) shared_fiber = fiber->shared_from_this_fiber();
        // Direct handoff slot: release() gives the connection to the waiting fiber HERE without putting it in the list.
        // It used to put the connection in the list and wake the waiter; until the woken fiber's
        // turn came a newly arrived request grabbed it, and the woken one went to the END of the queue — under load
        // the same request lost its turn again and again (the p99 tail at c=1000). While a fiber is suspended its
        // stack stays in place, the slot is valid.
        std::shared_ptr<DbConnection> handed;
        // Are we in the queue (under g_fiber_waiters_mtx; release/notify sets it false when removing the record).
        // A spurious wakeup (an early resume left over from another wait — pending_resume_) returns from suspension while our record is still
        // in the queue: it used to be that the loop added itself to the queue a SECOND time, the old
        // record stayed, and after acquire returned a release could write the connection to the dead stack
        // slot in that record and wake an unrelated fiber. Now if we are in the queue we keep waiting.
        bool queued = false;

        while (true) {
            bool grow = false;
            {
                // The decision is under the SAME pool lock: an idle connection → take it; if there is room → reserve a place, open it outside the lock;
                // otherwise wait. The waiting fiber's entry to the queue is under this lock too: so that a release()
                // arriving in between does not miss it and lose the wakeup. Lock order mtx →
                // g_fiber_waiters_mtx (release() takes them in the same order).
                std::unique_lock<std::mutex> lk(mtx);
                if (!available.empty()) {
                    auto c = available.back(); available.pop_back();
                    return c;
                }
                if (opener && (int)all.size() + opening < max_size) {
                    ++opening; grow = true;
                } else if (shared_fiber) {
                    std::lock_guard<std::mutex> wl(g_fiber_waiters_mtx);
                    g_fiber_waiters.push_back({shared_fiber, sched, this, &handed, &queued});
                    queued = true;
                } else {
                    // Thread context: a blocking wait (cv.wait is safe here)
                    cv.wait(lk, [this] {
                        return !available.empty() || (opener && (int)all.size() + opening < max_size);
                    });
                    continue;
                }
            }
            if (grow) {
                std::shared_ptr<DbConnection> c;
                try {
                    c = opener();   // network I/O: outside the lock (the fiber may be suspended here)
                } catch (...) {
                    { std::lock_guard<std::mutex> lk(mtx); --opening; }
                    notify_fiber_waiters(this);   // the reserved place is free again: let a waiter try itself
                    cv.notify_one();
                    throw;
                }
                std::lock_guard<std::mutex> lk(mtx);
                --opening;
                all.push_back(c);
                return c;
            }
            for (;;) {
                look::FiberScheduler::suspend_current();   // release() or a failed open wakes it
                std::lock_guard<std::mutex> wl(g_fiber_waiters_mtx);
                if (!queued) break;                        // whoever woke us removed our record
            }
            if (handed) return std::move(handed);      // the connection was handed directly to us
            // If there is no handoff a failed open woke us (the reserved place is free): try again from the start.
        }
    }

    void release(std::shared_ptr<DbConnection> c) {
        std::shared_ptr<look::Fiber> wake;
        look::FiberScheduler* wake_sched = nullptr;
        {
            // The lock order is the same as acquire: mtx → g_fiber_waiters_mtx.
            std::lock_guard<std::mutex> lk(mtx);
            {
                std::lock_guard<std::mutex> wl(g_fiber_waiters_mtx);
                // To the longest waiter (FIFO). Measured: giving first to the waiter on the same thread
                // is worse (that fiber runs only when the releasing fiber yields) — plain FIFO.
                for (auto it = g_fiber_waiters.begin(); it != g_fiber_waiters.end(); ++it) {
                    if (it->pool != this) continue;
                    *it->slot  = std::move(c);   // direct handoff
                    *it->queued = false;
                    wake       = std::move(it->fiber);
                    wake_sched = it->sched;
                    g_fiber_waiters.erase(it);
                    break;
                }
            }
            if (!wake) available.push_back(std::move(c));
        }
        if (wake) { wake_sched->resume_fiber(std::move(wake)); return; }
        cv.notify_one();   // no waiting fiber: wake the waiter of the blocking thread
    }

private:
    // ── Global fiber waiter queue (cross-scheduler safe) ─────────────────────
    // Each entry stores the fiber, its owning scheduler, and the pool it waits
    // on so notify_fiber_waiters() can route the resume to the right thread.
    struct FiberWaiter {
        std::shared_ptr<look::Fiber>  fiber;
        look::FiberScheduler*          sched;
        ConnPool*                      pool;
        std::shared_ptr<DbConnection>* slot;   // release() hands the connection over here
        bool*                          queued; // whoever removes the record sets it to false (under g_fiber_waiters_mtx)
    };

    static std::mutex               g_fiber_waiters_mtx;
    static std::vector<FiberWaiter> g_fiber_waiters;

    static void notify_fiber_waiters(ConnPool* pool) {
        // Wake the first waiter OF THIS POOL — the fiber's while(true) acquire
        // loop re-queues itself if the slot is stolen before it runs.
        // (Waking another pool's waiter would put this pool's waiter to sleep forever.)
        std::lock_guard<std::mutex> lk(g_fiber_waiters_mtx);
        for (auto it = g_fiber_waiters.begin(); it != g_fiber_waiters.end(); ++it) {
            if (it->pool != pool) continue;
            *it->queued = false;
            it->sched->resume_fiber(std::move(it->fiber));
            g_fiber_waiters.erase(it);
            return;
        }
    }
};

std::mutex               ConnPool::g_fiber_waiters_mtx;
std::vector<ConnPool::FiberWaiter> ConnPool::g_fiber_waiters;

static std::map<std::string, std::shared_ptr<ConnPool>> g_pools;
static std::mutex                                        g_pools_mtx;
static int                                               g_pool_seq  = 0;
static std::atomic<int>                                  g_pool_size{0}; // 0 = auto

// Per-request connection map: pool_key → borrowed conn
//
// Two-level lookup — fiber-aware:
//   1. If running inside a fiber: use fiber's FiberLocal storage
//      → Each fiber has its own connection map, no cross-fiber contamination
//   2. If running in thread context (no active fiber): use thread_local
//      → Backward compatible with non-fiber dispatch path
//
// This replaces the previous single thread_local tl_conns which was unsafe
// when multiple fibers shared a thread (fiber A sets conn, yields, fiber B
// overwrites it — fiber A resumes with wrong connection).

// fiber-ok: only a backup outside a fiber; inside a fiber FiberLocal::conns
static thread_local std::map<std::string, std::shared_ptr<DbConnection>> tl_conns;

// Returns the active connection map for the current execution context.
// Called on every db::query() — must be fast (no allocation on hot path).
static inline std::map<std::string, std::shared_ptr<DbConnection>>& active_conns() {
    look::Fiber* fiber = look::Fiber::current();
    if (fiber) return fiber->local.conns;   // fiber context — per-fiber storage
    return tl_conns;                         // thread context — legacy path
}

void set_db_pool_size(int n) { g_pool_size.store(n > 0 ? n : 0); }

// For runtime::stats: the total connection count of all DB pools + the busy (checked-out) count.
// busy = all - available (instantaneous under the pool mutex). Read-only observability.
void db_pool_stats(int& total_size, int& busy) {
    total_size = 0; busy = 0;
    std::vector<std::shared_ptr<ConnPool>> snap;
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        for (auto& [key, pool] : g_pools) if (pool) snap.push_back(pool);
    }
    for (auto& pool : snap) {
        std::lock_guard<std::mutex> plk(pool->mtx);
        int sz = (int)pool->all.size();
        int avail = (int)pool->available.size();
        total_size += sz;
        busy += (sz - avail);
    }
}

// Available CPUs — takes the cgroup CPU limit into account (container/systemd CPUQuota).
// hardware_concurrency() is physical; on a 2-CPU cgroup on an 8-core machine it opens 8 instead of
// 32 workers (the optimum found in the benchmark) → less RAM + the right CPU in a constrained environment.
int available_cpus() {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
#ifdef __linux__
    // The CPUs the process may run on (taskset, systemd CPUAffinity=, container cpuset): like Go's
    // GOMAXPROCS. hardware_concurrency() counts all the CPUs on the machine.
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            int n = CPU_COUNT(&set);
            if (n >= 1 && n < hw) hw = n;
        }
    }
    double c = -1.0;
    // cgroup v2: /sys/fs/cgroup/cpu.max = "quota period" (quota="max" → no limit)
    {
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string a, b;
        if (f && (f >> a >> b) && a != "max") {
            double q = std::atof(a.c_str());
            double p = b.empty() ? 100000.0 : std::atof(b.c_str());
            if (q > 0 && p > 0) c = q / p;
        }
    }
    // cgroup v1: cpu.cfs_quota_us / cpu.cfs_period_us
    if (c < 0) {
        std::ifstream fq("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us");
        std::ifstream fp("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us");
        long q = -1, p = -1;
        if (fq) fq >> q;
        if (fp) fp >> p;
        if (q > 0 && p > 0) c = (double)q / (double)p;
    }
    if (c > 0) {
        int lim = (int)(c + 0.5);       // round to the nearest whole CPU
        if (lim < 1) lim = 1;
        if (lim < hw) hw = lim;          // the smaller of the physical and the cgroup
    }
#endif
    return hw;
}

// How many CPUs the CFS quota is (rounded), -1 if there is no quota/unlimited. A separate form of the quota-reading part of
// available_cpus() — for the startup warning's "workers > quota" detection.
int cfs_quota_cpus() {
#ifdef __linux__
    double c = -1.0;
    {
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string a, b;
        if (f && (f >> a >> b) && a != "max") {
            double q = std::atof(a.c_str());
            double p = b.empty() ? 100000.0 : std::atof(b.c_str());
            if (q > 0 && p > 0) c = q / p;
        }
    }
    if (c < 0) {
        std::ifstream fq("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us");
        std::ifstream fp("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us");
        long q = -1, p = -1;
        if (fq) fq >> q;
        if (fp) fp >> p;
        if (q > 0 && p > 0) c = (double)q / (double)p;
    }
    if (c > 0) { int lim = (int)(c + 0.5); return lim < 1 ? 1 : lim; }
#endif
    return -1;
}

// The connection is NOT taken up front at the start of a request: on the first db:: call get_conn() takes it from the pool and
// it stays bound to that request until it ends (release_thread_connections) — the same operation on the same
// connection. It used to take a connection from every pool at the start of every request: when the pool was full
// even a route that did NOT TOUCH the database waited for a connection (4 long queries + pool 4 →
// a route returning plain text ~1 s).
void release_thread_connections() {
    auto& conns = active_conns();
    // If a request ended with an open transaction (db::begin then an error, or a commit was forgotten) the connection went back to the pool
    // WITH THAT TRANSACTION: the next request ran inside it, with its own "commit" it made
    // what the failed request wrote permanent; on SQLite the write gate stayed held and other writes
    // dropped after 10 s. Now it is rolled back; if it cannot be rolled back the connection is closed (the pool opens
    // a new one). BEFORE the lock: ROLLBACK is a network call, the fiber may be suspended.
    for (auto& [key, conn] : conns) {
        if (!conn || !conn->is_connected() || !conn->in_transaction()) { if (conn) conn->tx_depth = 0; continue; }
        look::Logger::instance().log(look::LogLevel::LOG_WARN, "DB",
            std::string("a request ended inside an open transaction (") + conn->driver_name() +
            "); it was rolled back. Call db::commit() or db::rollback(), or use db::transaction($conn, fn)");
        try { conn->query("ROLLBACK"); } catch (...) {}
        conn->tx_depth = 0;
        if (conn->in_transaction()) { try { conn->close(); } catch (...) {} }
    }
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        for (auto& [key, conn] : conns)
            if (auto it = g_pools.find(key); it != g_pools.end())
                it->second->release(conn);
    }
    conns.clear();
}

void clear_db_pools() {
    // Called before a hot reload — close all pools (under the exclusive lock)
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    g_pools.clear();
    // g_pool_seq is not reset: new pools get new keys, old closures do not go stale
    // (route_registry is rebuilt anyway)
}

// get_conn — returns the active connection for this request/fiber
static std::shared_ptr<DbConnection> get_conn(const Value& v) {
    if (v.type() != Value::STRING)
        throw std::runtime_error("db: invalid connection handle");
    const std::string& key = v.as_string();
    // Fiber-aware lookup: fiber context → FiberLocal, thread context → thread_local
    auto& conns = active_conns();
    auto tl_it = conns.find(key);
    if (tl_it != conns.end()) return tl_it->second;
    // Fallback: single-thread setup mode or direct-mode scripts
    std::shared_ptr<ConnPool> pool;
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        auto it = g_pools.find(key);
        if (it == g_pools.end())
            throw std::runtime_error("db: connection not found");
        pool = it->second;
    }
    // pool->acquire() is fiber-aware: yields cooperatively if exhausted (fiber
    // context) or blocks the thread (thread context).
    std::shared_ptr<DbConnection> raw = pool->acquire();
    conns[key] = raw;
    return raw;
}

// pool_key — sequential handle string
static std::string make_pool_key() {
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    return "__pool_" + std::to_string(g_pool_seq++);
}

// LOOK_DB_REQUIRE_TLS — an opt-in operator policy (see db_dsn.h db_refuse_plaintext).
// Read once per process; "1"/"true"/non-empty = on, "0"/"false"/empty = off.
static bool db_require_tls() {
    static const bool v = [] {
        const char* e = std::getenv("LOOK_DB_REQUIRE_TLS");
        if (!e || !*e) return false;
        std::string s(e);
        return s != "0" && s != "false";
    }();
    return v;
}
static std::runtime_error db_plaintext_refused(const std::string& host,
                                               const std::string& secure_scheme) {
    return std::runtime_error(
        "db: refusing plaintext connection to remote host '" + host +
        "' (LOOK_DB_REQUIRE_TLS is set). Use " + secure_scheme +
        ":// (encrypted+verified), or add ?insecure_plaintext=1 to this DSN to allow it, "
        "or unset LOOK_DB_REQUIRE_TLS.");
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
    if (dsn.substr(0, 11) == "postgres://"  || dsn.substr(0, 14) == "postgresql://" ||
        dsn.substr(0, 14) == "postgresqls://") {
        std::string user, pass, host = "127.0.0.1", db_name;
        int port = 5432;
        std::string scheme = parse_dsn_part(dsn, user, pass, host, port, db_name);
        // TLS: the postgresqls:// scheme OR the ?tls=... query param in the db name.
        // TLS is NEW in PG → verify is the RIGHT DEFAULT (postgresqls:// / ?tls=verify → verified,
        // ?tls=insecure → encrypted but not verified). Unlike mysql/redis it starts secure.
        // The TLS decision was moved to a pure seam (look/db_dsn.h) → the table test (pg_dsn_test) locks it
        // without a network; the verify-ca silent-plaintext regression (558d735) is positive-controlled in that test.
        bool tls = (scheme == "postgresqls");
        bool tls_verify = true;
        bool insecure_pt = false;
        { size_t q = db_name.find('?');
          if (q != std::string::npos) {
            std::string query = db_name.substr(q + 1);
            db_name = db_name.substr(0, q);
            look::pg_resolve_tls(query, scheme == "postgresqls", tls, tls_verify);
            insecure_pt = look::db_insecure_plaintext_opt(query);
          } }
        if (look::db_refuse_plaintext(db_require_tls(), tls, host, insecure_pt))
            throw db_plaintext_refused(host, "postgresqls");
        auto c = std::make_shared<PostgresClient>();
        if (tls) c->set_tls(true, tls_verify);
        c->connect(host, port, user, pass, db_name);
        return c;
    }
    std::string user, pass, host = "127.0.0.1", db_name;
    int port = 3306;
    std::string scheme = parse_dsn_part(dsn, user, pass, host, port, db_name);
    // TLS: the mysqls://.../mariadbs:// scheme OR the ?tls=1 / ?ssl=... query param in the db name.
    // ?tls=verify / ?ssl=verify → additionally certificate+hostname verification (against MITM).
    bool tls = (scheme == "mysqls" || scheme == "mariadbs");
    bool tls_verify = false;
    bool insecure_pt = false;
    { size_t q = db_name.find('?');
      std::string query;
      if (q != std::string::npos) {
        query = db_name.substr(q + 1);
        db_name = db_name.substr(0, q);   // strip the query string from the db name
      }
      // The TLS decision is in look/db_dsn.h (pure, table-testable).
      look::mysql_resolve_tls(query, tls, tls, tls_verify);
      insecure_pt = look::db_insecure_plaintext_opt(query); }
    if (look::db_refuse_plaintext(db_require_tls(), tls, host, insecure_pt))
        throw db_plaintext_refused(host, "mysqls");
    if (scheme == "mysqls")   scheme = "mysql";
    if (scheme == "mariadbs") scheme = "mariadb";
    if (scheme != "mysql" && scheme != "mariadb")
        throw std::runtime_error("db::connect() unsupported DSN scheme: " + scheme);
    DbConfig cfg;
    cfg.host = host; cfg.port = port; cfg.user = user;
    cfg.password = pass; cfg.database = db_name; cfg.tls = tls; cfg.tls_verify = tls_verify;
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
    auto arr = std::make_shared<ValueArray>();
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
// SQLite type constants
static bool sqlite_is_int(uint8_t t)   { return t == sqlite_type::INTEGER; }
static bool sqlite_is_float(uint8_t t) { return t == sqlite_type::FLOAT; }
static bool sqlite_is_null(uint8_t t)  { return t == sqlite_type::NUL; }
// PostgreSQL type constants
static bool pg_is_int(uint8_t t)       { return t == pg_type::INT; }
static bool pg_is_float(uint8_t t)     { return t == pg_type::FLOAT; }
static bool pg_is_null(uint8_t t)      { return t == pg_type::NUL; }
static bool pg_is_bool(uint8_t t)      { return t == pg_type::BOOL; }

static Value rows_to_value(const std::vector<DbRow>& rows) {
    auto result = std::make_shared<ValueArray>();
    for (const auto& row : rows) {
        // Store as assoc array: ["__assoc__", col0, val0, col1, val1, ...]
        auto row_arr = std::make_shared<ValueArray>();
        row_arr->push_back(Value::assoc_marker());
        for (auto& [k, dv] : row) {
            row_arr->push_back(Value(k));
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) {
                row_arr->push_back(Value());  // null
            } else if (pg_is_bool(dv.type)) {
                // PostgreSQL BOOL: "t" = true, "f" = false
                row_arr->push_back(Value(dv.str == "t" || dv.str == "true" || dv.str == "1"));
            } else if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { row_arr->push_back(Value((int64_t)std::stoll(dv.str))); }
                catch (...) { row_arr->push_back(Value(dv.str)); }
            } else if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                double d;
                if (parse_double(dv.str, d)) row_arr->push_back(Value(d));
                else row_arr->push_back(Value(dv.str));
            } else {
                row_arr->push_back(Value(dv.str));
            }
        }
        result->push_back(Value(row_arr));
    }
    return Value(result);
}

// SQL injection protection — the right escape per driver
static std::string escape_str(const std::string& s, const char* driver) {
    if (strcmp(driver, "mysql") == 0 || strcmp(driver, "mariadb") == 0)
        return MySQLClient::escape(s);
    // SQLite and PostgreSQL: standard SQL, escape with a single quote ''
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else           out += c;
    }
    return out;
}

// Value list → DbParam list — REAL native binding (instead of bind_params' escape-interpolation).
// The data NEVER enters the SQL text; the driver binds at the protocol level (sqlite3_bind_* / MySQL
// COM_STMT / PG extended-query). The type mapping is the same as bind_params'. The placeholder COUNT check is
// now driver-native (sqlite3_bind_parameter_count / PG-mysql server) → no backtick/'#' blind spot.
static std::vector<DbParam> values_to_db_params(const std::vector<Value>& params) {
    std::vector<DbParam> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        switch (p.type()) {
            case Value::NONE:  out.push_back(DbParam::null());                break;
            case Value::INT:   out.push_back(DbParam::from_int(p.as_int()));  break;
            case Value::FLOAT: {
                double d = p.as_float();
                // NaN/Infinity: sqlite/pg SILENTLY make it null, mysql gives "Out of range" —
                // the three are inconsistent + two are silent data loss (Front 2). LOOK is fail-loud on 0.0/0.0;
                // in a DB bind it is fail-loud too + CONSISTENT: a clean error (instead of a silent null).
                if (!std::isfinite(d))
                    throw std::runtime_error("db: cannot bind NaN/Infinity float as a parameter");
                out.push_back(DbParam::from_float(d));
                break;
            }
            case Value::BOOL:  out.push_back(DbParam::from_bool(p.as_bool())); break;
            default:           out.push_back(DbParam::from_text(p.to_string())); break;
        }
    }
    return out;
}

static Module make_db_module(ModuleHost* interp) {
    Module m;
    m.name = "db";

    // db::connect("mysql://user:pass@host/database")
    // db::connect("sqlite://blog.db")
    // Creates a connection pool; pool size = --workers (default: hardware_concurrency)
    m.functions["connect"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::connect() requires DSN string");
        std::string dsn = args[0].to_string();

        // Idempotency: return the existing pool's key for the same DSN.
        // Without this a db::connect() inside a route opens a new pool ON EVERY REQUEST
        // (N connections per request) → MySQL max_connections is exhausted.
        // (Caught by the regression suite: 14 of 24 requests failed.)
        static std::map<std::string, std::string> dsn_to_key;  // protected by g_pools_mtx
        {
            std::lock_guard<std::mutex> lk(g_pools_mtx);
            auto it = dsn_to_key.find(dsn);
            if (it != dsn_to_key.end() && g_pools.count(it->second))
                return Value(it->second);
        }

        bool is_sqlite = (dsn.substr(0, 9) == "sqlite://");
        // The pool's UPPER LIMIT — independent of the worker count. If LOOK_DB_POOL_SIZE is given, that (as is);
        // otherwise 4 × usable CPUs, 8..64 (under fiber dispatch one worker can make many queries
        // wait at once; since connections are opened lazily an unused limit is free).
        int sz = g_pool_size.load();
        if (sz <= 0) sz = std::max(8, std::min(64, look::available_cpus() * 4));
        if (is_sqlite) { if (sz > 4) sz = 4; }   // SQLite: WAL concurrent reads, at most 4

        // SQLite `:memory:` is private to each connection — in a multi-connection pool
        // (and in the model where each worker thread pins its own connection) a table created
        // on one connection at setup does NOT EXIST on the others ("no such table").
        // The fix: redirect :memory: to a process-lifetime temporary FILE — all pool
        // connections share the same DB (the table is visible), pool=N is kept. Thread
        // safety comes NOT from per-connection separation but from the global statics protected by
        // SQLITE_THREADSAFE=2 (see CMakeLists; the old =0 comment was wrong).
        // This is the "single shared temporary DB" behavior the user expects from :memory:.
        // The idempotency key stays the ORIGINAL dsn; only the path that is opened changes.
        // S3: the path is inside a process-lifetime PRIVATE directory. The old look_mem_<pid>.db path
        // was in a predictable + world-writable /tmp → a local attacker could plant a symlink in advance
        // and use TOCTOU (std::remove then open) to make it write to another file (shared
        // hosting). Fix: open the directory with mkdtemp (POSIX, atomic 0700 + owner-only) / an unpredictable
        // name (Windows, temp already has a user-private ACL), put the DB INSIDE it. Since the directory
        // is ours and 0700, the file inside does not need O_EXCL|O_NOFOLLOW —
        // an attacker cannot plant a symlink in a private directory that does not exist. Create once, the pool shares it.
        std::string effective_dsn = dsn;
        if (is_sqlite && dsn.find(":memory:") != std::string::npos) {
            static std::mutex memdb_mtx;
            static std::string memdb_path;
            {
                std::lock_guard<std::mutex> lk(memdb_mtx);
                if (memdb_path.empty()) {
                    std::error_code ec;
                    auto tmp = std::filesystem::temp_directory_path(ec);
#ifdef _WIN32
                    std::random_device rd;
                    char nm[48];
                    std::snprintf(nm, sizeof(nm), "look_mem_%08x%08x%08x",
                                  (unsigned)rd(), (unsigned)rd(), (unsigned)rd());
                    auto dir = tmp / nm;
                    if (!std::filesystem::create_directory(dir, ec))
                        throw std::runtime_error(":memory: could not open a safe temp directory");
                    memdb_path = (dir / "db.sqlite").string();
#else
                    std::string tpl = (tmp / "look_mem_XXXXXX").string();
                    if (::mkdtemp(tpl.data()) == nullptr)
                        throw std::runtime_error(":memory: could not open a safe temp directory (mkdtemp)");
                    memdb_path = tpl + "/db.sqlite";
#endif
                }
            }
            effective_dsn = "sqlite://" + memdb_path;
            Logger::instance().log(LogLevel::LOG_INFO, "DB",
                ":memory: → redirected to a process-lifetime PRIVATE (0700) temp file");
        }

        auto pool = std::make_shared<ConnPool>();
        pool->dsn = effective_dsn;
        pool->max_size = sz;
        // The rest are opened as needed with the same DSN and options.
        pool->opener = [effective_dsn, args]() { return open_one_connection(effective_dsn, args); };
        // The first connection NOW: let a wrong password / an unreachable server stop the application's startup
        // (not on the first request). Like sql.Open + Ping in Go.
        {
            auto c = open_one_connection(effective_dsn, args);
            pool->all.push_back(c);
            pool->available.push_back(c);
        }

        std::string key = make_pool_key();
        {
            std::lock_guard<std::mutex> lk(g_pools_mtx);
            // Race: if another thread registered the same DSN before us use its
            // — the connections we just opened are closed at the end of the scope.
            auto it = dsn_to_key.find(dsn);
            if (it != dsn_to_key.end() && g_pools.count(it->second))
                return Value(it->second);
            g_pools[key]    = pool;
            dsn_to_key[dsn] = key;
        }
        Logger::instance().log(LogLevel::LOG_INFO, "DB",
            "Pool created (up to " + std::to_string(sz) + " connections, opened as needed): " + look::db_dsn_redact(dsn));

        // S4: DB-TLS security warning — ONCE PER POOL (not per query → no spam).
        // Everyone on the network feeds bytes to the hand-written wire parser; the user must know the encryption/verification
        // state and see WHAT TO DO. After the 2026-08-11 flip: for MySQL/Redis
        // verify=true is SECURE-BY-DEFAULT → warn only on EXPLICIT plaintext or the ?tls=insecure opt-out.
        // The decision is in the pure seam (look/db_dsn.h db_tls_warning) — one to one with the real TLS resolvers.
        {
            std::string w = look::db_tls_warning(dsn);
            if (!w.empty())
                Logger::instance().log(LogLevel::LOG_WARN, "DB-TLS", w);
        }
        return Value(key);
    };

    // db::query($conn, "SELECT ...", [$p1, $p2])
    m.functions["query"] = [interp](auto args) -> Value {
        // 3a: during the setup pass, db::query silently returns NO rows (the read path is
        // not serving yet) — even for data a setup-time db::exec just wrote. db::exec WORKS
        // at setup (writes persist, visible at request-time), so this guard is query-only.
        // Fail loud so an app that builds state from a top-level query isn't silently empty;
        // move the read into a route handler (request-time), where db::query works.
        if (interp && interp->is_setup_mode())
            throw std::runtime_error("db::query() returns no rows during the setup pass — the read "
                "path is not serving while routes are registered (db::exec writes, but a setup-time "
                "query reads empty). Move the query into a route handler (request-time). "
                "(This used to fail silently; see FRICTION #3a.)");
        if (args.size() < 2) throw std::runtime_error("db::query() requires connection and SQL");
        auto conn = get_conn(args[0]);
        std::string sql = args[1].to_string();
        std::vector<Value> params;
        if (args.size() >= 3 && args[2].type() == Value::ARRAY)
            params = *args[2].as_array();
        auto rows = conn->execute(sql, values_to_db_params(params));   // real native binding
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
        conn->execute(sql, values_to_db_params(params));   // real native binding
        return Value((int64_t)conn->affected_rows());
    };

    // db::last_id($conn) — uses thread-local connection (same as exec in this request)
    m.functions["last_id"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::last_id() requires connection");
        return Value((int64_t)get_conn(args[0])->last_insert_id());
    };

    // db::affected($conn)
    m.functions["affected"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::affected() requires connection");
        return Value((int64_t)get_conn(args[0])->affected_rows());
    };

    // db::driver($conn) — "sqlite" | "mysql" | "postgres". Lets dialect-aware code
    // (e.g. the migrate module's schema→DDL) pick the right SQL for the live DB.
    m.functions["driver"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::driver() requires connection");
        return Value(std::string(get_conn(args[0])->driver_name()));
    };

    // db::escape($conn, $str) — DRIVER-AWARE. It used to use MySQL
    // backslash escaping (`\'`) for EVERY driver; but in standard SQL on SQLite/PostgreSQL
    // a backslash is not an escape → the `'` in `\'` would close the string and open an
    // INJECTION. escape_str picks the right escaper per driver
    // (MySQL: backslash; SQLite/PG: `''` doubling).
    m.functions["escape"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::escape() requires connection and string");
        return Value(escape_str(args[1].to_string(), get_conn(args[0])->driver_name()));
    };

    // db::close($conn) — removes the pool; existing borrowed conns complete normally
    m.functions["close"] = [](auto args) -> Value {
        if (args.empty()) return Value();
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        g_pools.erase(args[0].to_string());
        return Value();
    };

    // db::begin/commit/rollback — manual transaction control
    m.functions["begin"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::begin() requires connection");
        auto c = get_conn(args[0]);
        c->query(c->begin_stmt());   // SQLite: BEGIN IMMEDIATE (D-01)
        return Value();
    };
    m.functions["commit"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::commit() requires connection");
        get_conn(args[0])->query("COMMIT");
        return Value();
    };
    m.functions["rollback"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::rollback() requires connection");
        get_conn(args[0])->query("ROLLBACK");
        return Value();
    };

    // db::transaction($conn, fn) — a wrapper with nested SAVEPOINT support
    m.functions["transaction"] = [interp](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::transaction() requires connection and function");
        auto conn = get_conn(args[0]);
        const bool nested = (conn->tx_depth > 0);
        const std::string sp = nested ? ("look_sp_" + std::to_string(conn->tx_depth)) : "";
        if (nested) {
            conn->query("SAVEPOINT " + sp);
        } else {
            conn->query(conn->begin_stmt());   // SQLite: BEGIN IMMEDIATE (D-01)
        }
        conn->tx_depth++;
        try {
            // invoke() handles both a FUNCTION and a VM closure (BYTECODE_FN → bridge).
            // The old "FUNCTION only" guard SILENTLY skipped the VM closure and did COMMIT
            // (commit without the transaction body ever running → data loss). Right now
            // the fallback masks it (db::transaction was not a builtin) but it would have triggered when
            // added as a builtin. A non-function argument throws an error inside invoke
            // → the catch below → ROLLBACK (instead of a silent commit).
            Value result = interp->invoke(args[1], {});
            conn->tx_depth--;
            if (nested) {
                conn->query("RELEASE SAVEPOINT " + sp);
            } else {
                conn->query("COMMIT");
            }
            return result;
        } catch (...) {
            conn->tx_depth--;
            try {
                if (nested) {
                    conn->query("ROLLBACK TO SAVEPOINT " + sp);
                    conn->query("RELEASE SAVEPOINT " + sp);
                } else {
                    conn->query("ROLLBACK");
                }
            } catch (...) {}
            throw;
        }
    };

    // db::col($conn, $sql, $params) → the first column of the first row
    // db::col($row, $index_or_name) → a column value from a row
    m.functions["col"] = [](auto args) -> Value {
        if (args.size() < 2) return Value();

        // Connection + SQL execution mode
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
            auto rows = conn->execute(sql, values_to_db_params(params));   // real native binding
            if (rows.empty() || rows[0].empty()) return Value();
            // Return the first column of the first row
            const auto& dv = rows[0][0].second;
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) return Value();
            if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { return Value((int64_t)std::stoll(dv.str)); } catch (...) {}
            }
            if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                double d;
                if (parse_double(dv.str, d)) return Value(d);
            }
            return Value(dv.str);
        }

        if (args[0].type() != Value::ARRAY) return Value();
        auto& arr = *args[0].as_array();

        // Assoc array row: ["__assoc__", col0, val0, col1, val1, ...]
        if (!arr.empty() && arr[0].is_assoc_marker()) {
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

std::map<std::string, Module> make_web_modules(WebContext* fixed_ctx, ModuleHost* interp) {
    WebRef ctx{fixed_ctx};   // nullptr: the request at call time (look/request_scope.h)
    std::map<std::string, Module> mods;
    auto add = [&](Module mod) { mods[mod.name] = std::move(mod); };
    add(make_request(ctx));
    add(make_response_module(ctx));
    add(make_json_module());
    add(make_cookie_module(ctx));
    add(make_session_module(ctx));
    add(make_csrf_module(ctx));
    add(make_route_module(ctx));
    add(make_db_module(interp));
    return mods;
}

} // namespace look


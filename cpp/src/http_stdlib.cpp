// http_stdlib.cpp — LOOK `use http;` module binding
#include "look/interpreter.h"
#include "look/http_client.h"
#include <sstream>
#include <stdexcept>

namespace look {

// Build Value assoc-array from HttpResponse
static Value response_to_value(const HttpResponse& resp) {
    auto arr = std::make_shared<std::vector<Value>>();
    arr->push_back(Value(std::string("__assoc__")));  // sentinel

    arr->push_back(Value(std::string("status")));
    arr->push_back(Value((int)resp.status));

    arr->push_back(Value(std::string("body")));
    arr->push_back(Value(resp.body));

    // headers as nested assoc array
    auto hdrs = std::make_shared<std::vector<Value>>();
    hdrs->push_back(Value(std::string("__assoc__")));
    for (auto& [k, v] : resp.headers) {
        hdrs->push_back(Value(k));
        hdrs->push_back(Value(v));
    }
    arr->push_back(Value(std::string("headers")));
    arr->push_back(Value(hdrs));

    arr->push_back(Value(std::string("error")));
    if (resp.error.empty()) arr->push_back(Value());           // null
    else                    arr->push_back(Value(resp.error));

    return Value(arr);
}

// Extract headers map from LOOK assoc Value (optional arg)
static std::map<std::string, std::string> extract_headers(const Value& v) {
    std::map<std::string, std::string> out;
    if (v.type() != Value::ARRAY) return out;
    auto& arr = *v.as_array();
    // starts at 1 (skip __assoc__)
    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
        out[arr[i].to_string()] = arr[i+1].to_string();
    }
    return out;
}

// Extract options map from LOOK assoc Value (optional arg)
static HttpOptions extract_opts(const Value& v) {
    HttpOptions opts;
    if (v.type() != Value::ARRAY) return opts;
    auto& arr = *v.as_array();
    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
        std::string k = arr[i].to_string();
        if (k == "timeout") opts.timeout_ms = (int)arr[i+1].to_int();
    }
    return opts;
}

Module make_http_module() {
    Module m;
    m.name = "http";

    // http::get($url [, $headers [, $opts]])
    m.functions["get"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("http::get() — URL gerekli");
        std::string url = args[0].to_string();
        auto hdrs = args.size() >= 2 ? extract_headers(args[1]) : std::map<std::string,std::string>{};
        auto opts = args.size() >= 3 ? extract_opts(args[2]) : HttpOptions{};
        return response_to_value(http_request("GET", url, "", hdrs, opts));
    };

    // http::post($url, $body [, $headers [, $opts]])  — form-encoded body
    m.functions["post"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("http::post() — URL ve body gerekli");
        std::string url  = args[0].to_string();
        std::string body = args[1].to_string();
        auto hdrs = args.size() >= 3 ? extract_headers(args[2]) : std::map<std::string,std::string>{};
        auto opts = args.size() >= 4 ? extract_opts(args[3]) : HttpOptions{};
        return response_to_value(http_request("POST", url, body, hdrs, opts));
    };

    // http::post_json($url, $data [, $headers [, $opts]])  — auto json::encode
    m.functions["post_json"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("http::post_json() — URL ve data gerekli");
        std::string url = args[0].to_string();

        // Serialize the second arg to JSON manually
        // We reuse the same JSON encoder as json::encode (value_to_json)
        std::function<std::string(const Value&)> to_json = [&](const Value& v) -> std::string {
            if (v.type() == Value::NONE)   return "null";
            if (v.type() == Value::BOOL)   return v.as_bool() ? "true" : "false";
            if (v.type() == Value::INT)    return std::to_string(v.to_int());
            if (v.type() == Value::FLOAT) {
                std::ostringstream ss;
                ss << v.to_float();
                std::string s = ss.str();
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
                return s;
            }
            if (v.type() == Value::STRING) {
                std::string s = v.as_string();
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"')  out += "\\\"";
                    else if (c == '\\') out += "\\\\";
                    else if (c == '\n') out += "\\n";
                    else if (c == '\r') out += "\\r";
                    else if (c == '\t') out += "\\t";
                    else out += c;
                }
                return out + "\"";
            }
            if (v.type() == Value::ARRAY) {
                auto& arr = *v.as_array();
                bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING
                                && arr[0].as_string() == "__assoc__";
                if (is_assoc) {
                    std::string out = "{";
                    bool first = true;
                    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                        if (!first) out += ",";
                        out += "\"" + arr[i].to_string() + "\":";
                        out += to_json(arr[i+1]);
                        first = false;
                    }
                    return out + "}";
                } else {
                    std::string out = "[";
                    for (size_t i = 0; i < arr.size(); ++i) {
                        if (i) out += ",";
                        out += to_json(arr[i]);
                    }
                    return out + "]";
                }
            }
            return "null";
        };

        std::string body = to_json(args[1]);

        auto hdrs = args.size() >= 3 ? extract_headers(args[2]) : std::map<std::string,std::string>{};
        hdrs["Content-Type"] = "application/json";   // force JSON content type
        auto opts = args.size() >= 4 ? extract_opts(args[3]) : HttpOptions{};
        return response_to_value(http_request("POST", url, body, hdrs, opts));
    };

    // http::put($url, $body [, $headers [, $opts]])
    m.functions["put"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("http::put() — URL ve body gerekli");
        std::string url  = args[0].to_string();
        std::string body = args[1].to_string();
        auto hdrs = args.size() >= 3 ? extract_headers(args[2]) : std::map<std::string,std::string>{};
        auto opts = args.size() >= 4 ? extract_opts(args[3]) : HttpOptions{};
        return response_to_value(http_request("PUT", url, body, hdrs, opts));
    };

    // http::delete($url [, $headers [, $opts]])
    m.functions["delete"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("http::delete() — URL gerekli");
        std::string url = args[0].to_string();
        auto hdrs = args.size() >= 2 ? extract_headers(args[1]) : std::map<std::string,std::string>{};
        auto opts = args.size() >= 3 ? extract_opts(args[2]) : HttpOptions{};
        return response_to_value(http_request("DELETE", url, "", hdrs, opts));
    };

    // http::patch($url, $body [, $headers [, $opts]])
    m.functions["patch"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("http::patch() — URL ve body gerekli");
        std::string url  = args[0].to_string();
        std::string body = args[1].to_string();
        auto hdrs = args.size() >= 3 ? extract_headers(args[2]) : std::map<std::string,std::string>{};
        auto opts = args.size() >= 4 ? extract_opts(args[3]) : HttpOptions{};
        return response_to_value(http_request("PATCH", url, body, hdrs, opts));
    };

    return m;
}

} // namespace look

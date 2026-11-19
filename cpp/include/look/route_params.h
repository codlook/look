#pragma once
// Route path parameters are passed to the handler BY NAME (single definition).
//
//   route("GET", "/c/{id}/{slug}", function($slug) { ... })     -> $slug = the slug in the path
//
// They used to be matched by position: function($slug) in the route above would silently receive the
// value of `id`; an extra parameter would silently be null. Now:
//   * each handler parameter receives the value of the route path parameter with the same name;
//   * a name that is not in the route is an ERROR (at route registration -- at load -- and at call time);
//   * the handler may declare all, some or none of the path parameters;
//     the ones it does not declare are read with request::param("name").
// In WS and SSE routes the first parameter is the connection itself (any name); the rule applies to the rest.
#include "look/runtime_host.h"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace look {

inline std::string route_bare(const std::string& n) { return (!n.empty() && n[0] == '$') ? n.substr(1) : n; }

// "/c/{id}/{slug}" -> ["id", "slug"]
inline std::vector<std::string> route_placeholders(const std::string& pattern) {
    std::vector<std::string> out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '{') continue;
        size_t e = pattern.find('}', i);
        if (e == std::string::npos) break;
        out.push_back(pattern.substr(i + 1, e - i - 1));
        i = e;
    }
    return out;
}

inline size_t route_handler_skip(const std::string& method) { return (method == "WS" || method == "SSE") ? 1 : 0; }

[[noreturn]] inline void route_param_error(const std::string& method, const std::string& pattern,
                                           const std::string& param, const std::vector<std::string>& names) {
    std::string have;
    for (const auto& n : names) have += (have.empty() ? "" : ", ") + n;
    throw std::runtime_error("route " + method + " " + pattern + ": handler parameter $" + param
        + " is not a path parameter of this route ("
        + (names.empty() ? std::string("it has none") : "it has: " + have)
        + "); a handler parameter takes the path parameter of the same name");
}

// At registration: does every name of the handler (other than the connection parameter) exist in the route?
inline void route_check_handler(const std::string& method, const std::string& pattern,
                                const std::vector<std::string>& handler_params) {
    if (method == "404") return;
    const auto names = route_placeholders(pattern);
    for (size_t i = route_handler_skip(method); i < handler_params.size(); ++i) {
        const std::string p = route_bare(handler_params[i]);
        bool found = false;
        for (const auto& n : names) if (n == p) { found = true; break; }
        if (!found) route_param_error(method, pattern, p, names);
    }
}

// At call time: values for the handler's parameters (after skip), BY NAME.
template <class Map>
inline void route_args_by_name(const std::string& method, const std::string& pattern,
                               const std::vector<std::string>& handler_params, size_t skip,
                               const Map& route_params, std::vector<Value>& args) {
    for (size_t i = skip; i < handler_params.size(); ++i) {
        const std::string p = route_bare(handler_params[i]);
        auto it = route_params.find(p);
        if (it == route_params.end()) route_param_error(method, pattern, p, route_placeholders(pattern));
        args.push_back(Value(std::string(it->second)));
    }
}

} // namespace look

#pragma once
// LOOK 2 — rota yol parametreleri işleyiciye ADA göre verilir (iki motorda tek tanım).
//
//   route("GET", "/c/{id}/{slug}", function($slug) { ... })     → $slug = yoldaki slug
//
// Eskiden sıraya göre eşleşiyordu: function($slug) yukarıdaki rotada sessizce `id` değerini
// alırdı; fazladan yazılan parametre de sessizce null olurdu. Artık:
//   * işleyicinin her parametresi, rotanın aynı adlı yol parametresinin değerini alır;
//   * rotada olmayan bir ad HATADIR (rota kaydedilirken — yüklemede — ve çağrılırken);
//   * işleyici yol parametrelerinin hepsini, bir kısmını ya da hiçbirini bildirebilir;
//     bildirmedikleri request::param("ad") ile okunur.
// WS ve SSE rotalarında ilk parametre bağlantının kendisidir (adı serbest); kural kalanlara uygulanır.
#include "look/interpreter.h"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace look {

inline std::string route_bare(const std::string& n) { return (!n.empty() && n[0] == '$') ? n.substr(1) : n; }

// "/c/{id}/{slug}" → ["id", "slug"]
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

// Kayıt anında: işleyicinin (bağlantı parametresi dışındaki) her adı rotada var mı?
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

// Çağrı anında: işleyicinin (skip'ten sonraki) parametreleri için değerler, ADA göre.
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

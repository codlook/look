#pragma once
// The server's builtin health path (GET, returns "ok" without running the script). Default /health,
// moved with LOOK_HEALTH_PATH. Single source: HTTP and FCGI requests are answered here, and the startup
// check uses the same path to find a user route that collides with it.
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace look {

inline const std::string& health_path() {
    static const std::string p = [] {
        const char* e = std::getenv("LOOK_HEALTH_PATH");
        return std::string(e ? e : "/health");
    }();
    return p;
}

// If a user route collides with the builtin health path (GET + same path) that route NEVER runs --
// the server answers the request itself before the script. It used to be silently dead code (VS Code example);
// now the application does not start and the reason is stated. (A parameterized pattern, e.g. /{page}, only
// loses one value -- that is not a collision and is not checked.)
inline void route_check_builtin_path(const std::string& method, const std::string& pattern) {
    std::string m = method;
    for (auto& c : m) c = (char)std::toupper((unsigned char)c);
    if (m != "GET" || health_path().empty() || pattern != health_path()) return;
    throw std::runtime_error("route GET " + pattern + " is never reached: the server answers GET " + pattern +
        " itself (built-in health check) before the application runs. Use another path, or move the"
        " health check with LOOK_HEALTH_PATH.");
}

} // namespace look

#pragma once
// Sunucunun yerleşik sağlık yolu (GET, betik çalışmadan "ok" döner). Varsayılan /health,
// LOOK_HEALTH_PATH ile taşınır. Tek kaynak: HTTP ve FCGI isteği burada karşılar, başlangıç
// denetimi aynı yolla çakışan kullanıcı rotasını bununla bulur.
#include <cctype>
#include <cstdlib>
#include <string>

namespace look {

inline const std::string& health_path() {
    static const std::string p = [] {
        const char* e = std::getenv("LOOK_HEALTH_PATH");
        return std::string(e ? e : "/health");
    }();
    return p;
}

// Kullanıcı rotası yerleşik sağlık yoluyla çakışıyorsa (GET + aynı yol) o rota HİÇ çalışmaz —
// sunucu isteği betikten önce kendisi karşılar. Eskiden sessizce ölü koddu (VS Code örneği).
// v1: başlangıçta UYARI (bu mesaj); LOOK 2'de uygulama başlamaz. Çakışma yoksa boş döner.
// (Parametreli kalıp, ör. /{sayfa}, yalnız bir değeri kaybeder — o çakışma değil.)
inline std::string route_builtin_path_conflict(const std::string& method, const std::string& pattern) {
    std::string m = method;
    for (auto& c : m) c = (char)std::toupper((unsigned char)c);
    if (m != "GET" || health_path().empty() || pattern != health_path()) return "";
    return "route GET " + pattern + " is never reached: the server answers GET " + pattern +
        " itself (built-in health check) before the application runs. Use another path, or move the"
        " health check with LOOK_HEALTH_PATH.";
}

} // namespace look

#pragma once
// installer yönlendirme (redirect) güvenlik-kararı — saf predicate (installer.cpp download_follow
// "dikişi aç"). Gerçek HTTP/3xx OLMADAN tablo-test edilir; şema-düşürme + host-kaçışı regresyonu
// ağsız kilitlenir.
//
// GitHub zipball'ı codeload/objects alt-alanlarına yönlendirir → allowlist. Reddedilenler:
// http:// (şema düşürme, 48. bug) ve beklenmeyen host. ".github.com" sonek kontrolü NOKTA'yı
// zorunlu kılar → "evilgithub.com" ve "github.com.evil.com" reddedilir (öneksiz/yanlış-alan).
#include <string>
#include <cstring>

namespace look {

inline bool redirect_allowed(const std::string& next, std::string& err) {
    if (next.rfind("https://", 0) != 0) {
        err = "güvensiz yönlendirme (yalnız https): " + next;
        return false;
    }
    std::string rest = next.substr(8);              // "https://" sonrası
    size_t slash = rest.find('/');
    std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    auto ends_with = [](const std::string& s, const char* suf) {
        size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    bool ok = host == "github.com" || host == "api.github.com" ||
              ends_with(host, ".github.com") ||
              ends_with(host, ".githubusercontent.com");
    if (!ok) { err = "yönlendirme beklenmeyen host'a gidiyor: " + host; return false; }
    return true;
}

} // namespace look

#pragma once
// Güvenliği zayıflatan ayarlar — sunucu başlarken her biri için bir WARN satırı (davranış değişmez).
// Eskiden bu ayarlar sessizdi: geliştirme/test için açılan bir ayar üretimde unutulunca hiçbir
// iz bırakmıyordu. Saf işlev (ortam okuyucusu parametre) — tablo testi yapılabilir; çağıran yazar.
#include <cstdlib>
#include <string>
#include <vector>

namespace look {

// IPv4 "a.b.c.d/N": N < 8 ya da 0.0.0.0 → sıradan istemci adreslerini kapsar.
inline bool weak_proxy_entry(const std::string& e) {
    if (e == "0.0.0.0" || e == "::/0" || e == "*") return true;
    auto slash = e.find('/');
    if (slash == std::string::npos) return false;
    try { return std::stoi(e.substr(slash + 1)) < 8; } catch (...) { return false; }
}

inline std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != ' ' && c != '\t') cur += c;
    }
    out.push_back(cur);
    return out;
}

// get: const char* döndüren okuyucu (std::getenv ya da testte bir tablo).
template <class Get>
inline std::vector<std::string> weak_setting_warnings(Get get, bool fastcgi) {
    std::vector<std::string> w;
    auto val = [&get](const char* k) -> std::string { const char* v = get(k); return v ? v : ""; };

    if (val("LOOK_ALLOW_SSRF") == "1")
        w.push_back("LOOK_ALLOW_SSRF=1: http:: requests may reach this machine and the private network "
                    "(127.0.0.1, 10.0.0.0/8, 169.254.169.254 cloud metadata ...) at an address a visitor "
                    "supplies; leave it unset in production");
    if (val("LOOK_SESSION_SECURE") == "0")
        w.push_back("LOOK_SESSION_SECURE=0: the session and CSRF cookies go out without Secure, over HTTPS "
                    "too, so a plain-HTTP request can expose them; unset it to follow the connection");
    for (const auto& o : split_list(val("LOOK_WS_ORIGINS")))
        if (o == "*") {
            w.push_back("LOOK_WS_ORIGINS contains *: any web site can open a WebSocket to this server with "
                        "its visitors' cookies (cross-site WebSocket hijacking); list the allowed origins");
            break;
        }
    for (const auto& p : split_list(val("LOOK_TRUSTED_PROXY")))
        if (weak_proxy_entry(p)) {
            w.push_back("LOOK_TRUSTED_PROXY trusts " + p + ", which covers ordinary clients: any client can "
                        "set request::ip(), request::host() and request::scheme() with forwarded headers "
                        "(rate limits and logs follow); list only your proxy's addresses");
            break;
        }
    if (fastcgi) {
        const std::string b = val("LOOK_FCGI_BIND");
        if (!b.empty() && b.rfind("127.", 0) != 0)
            w.push_back("LOOK_FCGI_BIND=" + b + ": the FastCGI port accepts connections from other "
                        "machines, and FastCGI has no authentication — anyone who reaches it can send "
                        "requests with any parameters (HTTPS, REMOTE_ADDR, ...); bind to 127.0.0.1 or "
                        "firewall the port");
    }
    if (val("APP_ENV") == "development" || val("APP_DEBUG") == "true")
        w.push_back(std::string(val("APP_ENV") == "development" ? "APP_ENV=development" : "APP_DEBUG=true") +
                    ": error responses show error details to visitors; do not use it in production");
    return w;
}

} // namespace look

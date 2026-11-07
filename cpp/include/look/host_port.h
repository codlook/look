// host[:port] ve port ayrıştırma — TEK tanım (DSN, Redis URL, http:: URL, proxy, --port, *_PORT env).
// Model Go: net/url (boş port = varsayılan), net.SplitHostPort (IPv6 köşeli parantezde),
// strconv.ParseUint(s, 10, 16) (yalnız rakam, taşma = hata). Eskiden her yer kendi stoi/atoi'sini
// kullanıyordu: "3306x" → 3306 (stoi ilk harfte durur), "99999" kabul, "abc" → çıplak "stoi" hatası
// ya da sessizce varsayılan port, "[::1]:5432" → host "[" — IPv6 adresle hiç bağlanılamıyordu.
// Saf (ağsız) — tests/host_port_test.cpp tablo testi.
#pragma once
#include <string>

namespace look {

// Port metni: yalnız ASCII rakam, 1..65535. Hata → false.
inline bool parse_port_number(const std::string& s, int& out) {
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    out = v;
    return true;
}

// Kullanıcıya giden açıklama (İngilizce): "invalid port \"3306x\" (expected 1-65535)".
inline std::string port_error(const std::string& s) {
    return "invalid port \"" + s + "\" (expected a number from 1 to 65535)";
}

// "host", "host:port", "host:" (boş port = varsayılan), "[v6]", "[v6]:port".
// host: parantezsiz; boş kalırsa çağıranın varsayılanı korunur. port: girişte varsayılan.
// Dönüş "" = başarı; aksi halde İngilizce hata (port/adres metnini içerir, parola içermez).
inline std::string split_host_port(const std::string& hp, std::string& host, int& port) {
    std::string h, p;
    bool has_port = false;
    if (!hp.empty() && hp[0] == '[') {
        size_t rb = hp.find(']');
        if (rb == std::string::npos) return "missing ']' in IPv6 address \"" + hp + "\"";
        h = hp.substr(1, rb - 1);
        if (h.empty()) return "empty IPv6 address in \"" + hp + "\"";
        if (rb + 1 < hp.size()) {
            if (hp[rb + 1] != ':') return "unexpected text after ']' in \"" + hp + "\"";
            p = hp.substr(rb + 2);
            has_port = true;
        }
    } else {
        size_t c = hp.find(':');
        if (c != std::string::npos && hp.find(':', c + 1) != std::string::npos)
            return "an IPv6 address must be in brackets, like [::1]:5432 (got \"" + hp + "\")";
        if (c != std::string::npos) { h = hp.substr(0, c); p = hp.substr(c + 1); has_port = true; }
        else h = hp;
    }
    int v = port;
    if (has_port && !p.empty() && !parse_port_number(p, v)) return port_error(p);
    if (!h.empty()) host = h;
    port = v;
    return "";
}

} // namespace look

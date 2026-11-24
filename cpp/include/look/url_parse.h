#pragma once
// URL parser of the HTTP client (http::get, redirects), a pure function (no network) so that
// tests/url_parse_referee_test.cpp can check it against Go's net/url on random URLs.
// Go net/url model — see the notes above parse_url.
#include "look/host_port.h"
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>

namespace look {

// Parses "https://host:port/path?query" into parts
struct ParsedUrl {
    bool        tls  = false;
    std::string host;
    int         port = 80;
    std::string path;   // includes query string
    std::string basic_auth;   // "user:pass@" in the URL → base64 for "Authorization: Basic" (Go URL.User)
};


inline std::string url_pct_decode(const std::string& s) {
    std::string o; o.reserve(s.size());
    auto hx = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hx(s[i + 1]) >= 0 && hx(s[i + 2]) >= 0) {
            o += (char)(hx(s[i + 1]) * 16 + hx(s[i + 2])); i += 2;
        } else o += s[i];
    }
    return o;
}

inline std::string url_base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

// Every '%' starts two hex digits (Go refuses a URL with a bad escape in the user info, host, path or fragment).
inline bool url_escapes_ok(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '%' && !(i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])))
            return false;
    return true;
}

// Go net/url model:
//   * the scheme is case-insensitive ("HTTPS://" is valid);
//   * the authority ends at the first '/', '?' or '#' — it used to be only '/': "http://h?q=1" → host "h?q=1";
//   * the fragment (#...) is not sent to the server (RFC 9110 §4.2.4);
//   * "user:pass@" → Basic credentials (percent-decoded; Go Request.URL.User); it used to get mixed into the host;
//   * port: digits only, 1-65535, empty = default; IPv6 in square brackets (look/host_port.h).
//     A malformed port used to silently fall back to 80/443 ("http://h:8O80/" → connect to 80).
//   * what Go refuses is refused: a control byte (CR/LF in the URL would have been written into the request line), a
//     user name with a character that cannot be in one, a host with a space, '\', '^' or another byte that cannot be in a
//     host name, a bad %-escape in the user info, path or fragment (tests/url_parse_referee_test.cpp checks it against Go
//     on random URLs: before, 4,211 of 30,000 were read where Go refused them). A host is letters, digits, '.', '-', '_',
//     '~' and bytes of 0x80 and over (Go also lets !$&'()*+,;=<>\" through; refusing them is stricter and safe). An IPv6
//     zone is written "%25" in a URL and read "%": "[fe80::1%25eth0]" connects to "fe80::1%eth0", as in Go.
//   * a space in the path is sent as %20 (Go does the same): a raw space would end the request line early.
inline ParsedUrl parse_url(const std::string& url) {
    ParsedUrl r;
    for (unsigned char c : url)
        if (c < 0x20 || c == 0x7f) throw std::runtime_error("http:: invalid URL: control character in URL");
    size_t se = url.find("://");
    std::string scheme = (se == std::string::npos) ? "" : url.substr(0, se);
    for (auto& c : scheme) c = (char)std::tolower((unsigned char)c);
    if (scheme == "https")     { r.tls = true;  r.port = 443; }
    else if (scheme == "http") { r.tls = false; r.port = 80; }
    else throw std::runtime_error("http:: Unsupported URL scheme: " + url);
    std::string s = url.substr(se + 3);

    size_t end = s.find_first_of("/?#");
    std::string auth = s.substr(0, end);
    std::string rest = (end == std::string::npos) ? "" : s.substr(end);
    size_t hash = rest.find('#');
    if (hash != std::string::npos) {
        if (!url_escapes_ok(rest.substr(hash + 1))) throw std::runtime_error("http:: invalid URL: bad escape in the fragment");
        rest.erase(hash);
    }
    if (!url_escapes_ok(rest.substr(0, rest.find('?')))) throw std::runtime_error("http:: invalid URL: bad escape in the path");
    for (size_t sp = rest.find(' '); sp != std::string::npos; sp = rest.find(' ', sp + 3)) rest.replace(sp, 1, "%20");
    if (rest.empty() || rest[0] == '?') rest = "/" + rest;
    r.path = rest;

    size_t at = auth.rfind('@');
    if (at != std::string::npos) {
        std::string ui = auth.substr(0, at);
        for (char c : ui) {
            bool ok = std::isalnum((unsigned char)c) || std::strchr("-._:~!$&'()*+,;=%@", c) != nullptr;
            if (!ok) throw std::runtime_error("http:: invalid URL: invalid character in the user info");
        }
        if (!url_escapes_ok(ui)) throw std::runtime_error("http:: invalid URL: bad escape in the user info");
        auth = auth.substr(at + 1);
        size_t c = ui.find(':');
        std::string user = url_pct_decode(c == std::string::npos ? ui : ui.substr(0, c));
        std::string pass = c == std::string::npos ? "" : url_pct_decode(ui.substr(c + 1));
        r.basic_auth = url_base64(user + ":" + pass);
    }
    std::string err = look::split_host_port(auth, r.host, r.port);
    if (!err.empty()) throw std::runtime_error("http:: invalid URL: " + err);
    if (r.host.empty()) throw std::runtime_error("http:: invalid URL: missing host: " + url);
    if (auth[0] == '[') {
        // IPv6: hex digits, ':' and '.', then an optional zone "%25name"
        std::string zone;
        size_t pct = r.host.find('%');
        std::string addr = r.host.substr(0, pct);
        for (char c : addr)
            if (!(std::isxdigit((unsigned char)c) || c == ':' || c == '.')) throw std::runtime_error("http:: invalid URL: invalid IPv6 address");
        if (pct != std::string::npos) {
            if (r.host.compare(pct, 3, "%25") != 0 || r.host.size() == pct + 3) throw std::runtime_error("http:: invalid URL: invalid IPv6 zone");
            for (size_t i = pct + 3; i < r.host.size(); ++i) {
                char c = r.host[i];
                if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '~' || c == '-')) throw std::runtime_error("http:: invalid URL: invalid IPv6 zone");
            }
            r.host = addr + "%" + r.host.substr(pct + 3);
        }
    } else {
        for (unsigned char c : r.host)
            if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~' || c >= 0x80))
                throw std::runtime_error("http:: invalid URL: invalid character in the host name");
    }
    return r;
}


} // namespace look

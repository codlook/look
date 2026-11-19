// host[:port] and port parsing — a SINGLE definition (DSN, Redis URL, http:: URL, proxy, --port, *_PORT env).
// Go model: net/url (empty port = default), net.SplitHostPort (IPv6 in square brackets),
// strconv.ParseUint(s, 10, 16) (digits only, overflow = error). Every place used to have its own stoi/atoi:
// "3306x" → 3306 (stoi stops at the first letter), "99999" accepted, "abc" → a bare "stoi" error
// or silently the default port, "[::1]:5432" → host "[" — an IPv6 address could never be connected to.
// Pure (no network) — table test in tests/host_port_test.cpp.
#pragma once
#include <string>

namespace look {

// Port text: ASCII digits only, 1..65535. Error → false.
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

// The explanation shown to the user (English): "invalid port \"3306x\" (expected 1-65535)".
inline std::string port_error(const std::string& s) {
    return "invalid port \"" + s + "\" (expected a number from 1 to 65535)";
}

// "host", "host:port", "host:" (empty port = default), "[v6]", "[v6]:port".
// host: without brackets; if it stays empty the caller's default is kept. port: default on entry.
// Returns "" = success; otherwise an English error (contains the port/address text, never the password).
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

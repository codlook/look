// look::parse_url (look/url_parse.h, what http::get and redirect following connect to) against Go's net/url.
// stdin: lines made by input_fuzz/url_parse_referee.go (fields in hex): url, status, tls, host, port, user,
// has-userinfo, pass. For every URL Go reads as an http or https URL with a host (status OK):
//   * LOOK reads another host, port, scheme or user name  -> FAIL (a program that checks the URL with one reader and
//     connects with the other is open to SSRF);
//   * LOOK refuses it (a bad port, an empty host)         -> counted: stricter than Go, safe;
// for a URL Go cannot parse (ERR) or does not take for an http(s) URL with a host:
//   * LOOK accepts it                                     -> FAIL: two readers of one text disagree.
// Build: g++ -std=c++17 -Iinclude tests/url_parse_referee_test.cpp -o /tmp/upt
#include "look/url_parse.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string unhex(const std::string& s) {
    std::string o;
    for (size_t i = 0; i + 1 < s.size(); i += 2) o += (char)std::stoi(s.substr(i, 2), nullptr, 16);
    return o;
}
static std::string show(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

int main() {
    std::string line;
    int total = 0, same = 0, differ = 0, accepts_bad = 0, stricter = 0, both_refuse = 0;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> f; std::stringstream ss(line); std::string p;
        while (std::getline(ss, p, '\t')) f.push_back(p);
        while (f.size() < 8) f.push_back("");
        std::string url = unhex(f[0]), status = f[1], tls = f[2], host = unhex(f[3]), port = f[4], user = unhex(f[5]),
                    has_user = f[6], pass = unhex(f[7]);
        ++total;
        bool ok = true; look::ParsedUrl r;
        try { r = look::parse_url(url); } catch (const std::exception&) { ok = false; }
        if (status != "OK") {
            if (ok) { if (++accepts_bad <= 10) std::printf("  ACCEPTS  %-48s Go: %s; LOOK reads host \"%s\"\n", show(url).c_str(), status.c_str(), show(r.host).c_str()); }
            else ++both_refuse;
            continue;
        }
        if (!ok) { if (++stricter <= 4) std::printf("  stricter %-48s LOOK refuses; Go reads host \"%s\" port %s\n", show(url).c_str(), show(host).c_str(), port.c_str()); continue; }
        std::string want_auth = has_user == "1" ? look::url_base64(user + ":" + pass) : "";
        std::string why;
        if (r.host != host) why += " host \"" + show(r.host) + "\" (Go \"" + show(host) + "\")";
        if (std::to_string(r.port) != std::to_string(std::atoi(port.c_str()))) why += " port " + std::to_string(r.port) + " (Go " + port + ")";
        if ((r.tls ? "1" : "0") != tls) why += " tls";
        if (r.basic_auth != want_auth) why += " userinfo";
        if (why.empty()) ++same;
        else if (++differ <= 12) std::printf("  DIFFERENT %-47s LOOK reads%s\n", show(url).c_str(), why.c_str());
    }
    std::printf("%d URLs: same reading %d, LOOK reads another host/port/user than Go %d, LOOK accepts what Go does not read as an "
                "http(s) URL with a host %d, LOOK stricter %d, both refuse %d\n", total, same, differ, accepts_bad, stricter, both_refuse);
    if (differ || accepts_bad) { std::printf("FAIL: parse_url reads URLs differently from Go's net/url\n"); return 1; }
    std::printf("PASS: parse_url reads what Go reads, and refuses what Go cannot read\n");
    return 0;
}

#pragma once
// Security decision for installer redirects -- a pure predicate (installer.cpp download_follow,
// "seam opened"). Table-tested WITHOUT real HTTP/3xx; the scheme-downgrade + host-escape regression
// is locked without a network.
//
// A GitHub zipball redirects to codeload/objects subdomains -> allowlist. Rejected:
// http:// (scheme downgrade, bug 48) and an unexpected host. The ".github.com" suffix check REQUIRES
// the DOT -> "evilgithub.com" and "github.com.evil.com" are rejected (no-prefix/wrong-domain).
#include <string>
#include <cstring>

namespace look {

inline bool redirect_allowed(const std::string& next, std::string& err) {
    if (next.rfind("https://", 0) != 0) {
        err = "unsafe redirect (https only): " + next;
        return false;
    }
    std::string rest = next.substr(8);              // after "https://"
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
    if (!ok) { err = "redirect goes to an unexpected host: " + host; return false; }
    return true;
}

} // namespace look

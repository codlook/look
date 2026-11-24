#pragma once
// Security decision for installer redirects -- a pure predicate (installer.cpp download_follow,
// "seam opened"). Table-tested WITHOUT real HTTP/3xx; the scheme-downgrade + host-escape regression
// is locked without a network.
//
// A GitHub zipball redirects to codeload/objects subdomains -> allowlist. Rejected:
// http:// (scheme downgrade, bug 48) and an unexpected host. The ".github.com" suffix check REQUIRES
// the DOT -> "evilgithub.com" and "github.com.evil.com" are rejected (no-prefix/wrong-domain).
//
// The authority (what follows "https://" up to the first '/', '?' or '#') is accepted only when it is plain:
// letters, digits, '.', '-', and one optional ":443" (or a bare ":"). A redirect from GitHub never carries a user
// name, an IPv6 address, an escaped host, a backslash or another port, and those are the shapes with which two
// readers of one text name two hosts: in "https://github.com:x@evil.com/" the user name is "github.com:x" and the
// host is evil.com, which Go's net/url and look::parse_url (http_client.cpp) read, and the old check did not (it cut at
// the first ':' of the whole text and allowed it). The rest of the URL must be printable ASCII and every '%' must
// start a valid escape: Go refuses a URL with a control byte or a bad escape, so a text Go cannot read is not followed.
// tests/url_redirect_referee_test.cpp checks the rule against Go on 20,000 random URLs.
#include <string>
#include <cstring>
#include <cctype>

namespace look {

inline bool redirect_allowed(const std::string& next, std::string& err) {
    if (next.rfind("https://", 0) != 0) {
        err = "unsafe redirect (https only): " + next;
        return false;
    }
    for (size_t i = 0; i < next.size(); ++i) {
        unsigned char c = (unsigned char)next[i];
        bool escape_ok = i + 2 < next.size() && std::isxdigit((unsigned char)next[i + 1]) &&
                         std::isxdigit((unsigned char)next[i + 2]);
        if (c <= 0x20 || c >= 0x7f || (c == '%' && !escape_ok)) {
            err = "redirect has a byte or an escape that a URL cannot hold: " + next;
            return false;
        }
    }
    std::string rest = next.substr(8);              // after "https://"
    size_t end = rest.find_first_of("/?#");
    std::string auth = (end == std::string::npos) ? rest : rest.substr(0, end);
    for (char c : auth) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '.' || c == '-' || c == ':';
        if (!plain) { err = "redirect goes to an unexpected host: " + auth; return false; }
    }
    std::string host = auth;
    size_t colon = auth.find(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        std::string port = auth.substr(colon + 1);
        if (!(port.empty() || port == "443")) { err = "redirect goes to an unexpected port: " + auth; return false; }
    }
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

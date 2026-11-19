// HTML text-content escaping — a single definition (html::escape and TemplateEngine both use it).
// B-08: backtick (`` ` ``) is also escaped — closes the JS template-literal `${...}` XSS.
// NOTE: html::attr is a SEPARATE concept (allowlist: escapes everything except alphanumerics+UTF-8,
// more robust for the attribute context) — it is not merged here. "Single structure" is for two
// copies of the SAME concept; escape and attr are two different concepts.
#pragma once
#include <string>

namespace look {

inline std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            case '`':  out += "&#96;";  break;   // B-08: JS template-literal XSS
            default:   out += (char)c;
        }
    }
    return out;
}

} // namespace look

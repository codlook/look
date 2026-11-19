// lk fmt -- source formatter. See look/format_src.h (design + guarantee).
#include "look/format_src.h"
#include "look/lexer.h"
#include "look/token.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>

namespace look {

namespace {

// Scan one line, with the carried string state. in_str: 0 = outside a string, otherwise the open quote.
// Returns: net depth change (delta), count of line-leading closers (leading_closers).
// in_str is updated by reference (multi-line string support).
struct ScanResult { int delta = 0; int leading_closers = 0; };

ScanResult scan_line(const std::string& s, char& in_str) {
    ScanResult r;
    bool leading = true;            // still counting the line-leading closers
    bool line_comment = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (line_comment) break;
        if (in_str) {
            if (c == '\\') { if (i + 1 < s.size()) ++i; continue; }  // escape
            if (c == in_str) in_str = 0;                              // string closed
            continue;
        }
        // outside a string
        if (c == '#') { line_comment = true; break; }
        if (c == '/' && i + 1 < s.size() && s[i+1] == '/') { line_comment = true; break; }
        if (c == '"' || c == '\'') { in_str = c; leading = false; continue; }
        if (c == '{' || c == '[' || c == '(') { r.delta++; leading = false; continue; }
        if (c == '}' || c == ']' || c == ')') {
            r.delta--;
            if (leading) r.leading_closers++;
            continue;
        }
        if (c != ' ' && c != '\t') leading = false;
    }
    return r;
}

std::string rstrip(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r");
    return e == std::string::npos ? "" : s.substr(0, e + 1);
}
std::string lstrip(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    return b == std::string::npos ? "" : s.substr(b);
}

// Are the non-trivia token sequences equal (meaning-preservation check)? The lexer already drops
// comments; whitespace is not a token either -> only real program tokens are compared.
bool same_tokens(const std::string& a, const std::string& b) {
    try {
        auto ta = Lexer(a).scan_tokens();
        auto tb = Lexer(b).scan_tokens();
        if (ta.size() != tb.size()) return false;
        for (size_t i = 0; i < ta.size(); ++i) {
            if (ta[i].type != tb[i].type) return false;
            if (ta[i].lexeme != tb[i].lexeme) return false;
        }
        return true;
    } catch (...) {
        return false;   // if one cannot be parsed, stay on the safe side
    }
}

} // namespace

std::string look_format_source(const std::string& src, bool* ok) {
    std::vector<std::string> lines;
    { std::stringstream ss(src); std::string ln; while (std::getline(ss, ln)) lines.push_back(ln); }

    std::string out;
    int depth = 0;
    char in_str = 0;
    int blank_run = 0;

    for (const std::string& raw : lines) {
        if (in_str) {
            // INSIDE a multi-line string -- keep the line AS IS (indentation is string data).
            out += raw; out += "\n";
            scan_line(raw, in_str);   // only update in_str
            blank_run = 0;
            continue;
        }
        std::string content = rstrip(lstrip(raw));
        if (content.empty()) {
            if (++blank_run <= 1) out += "\n";   // collapse consecutive blank lines into one
            continue;
        }
        blank_run = 0;

        char probe = in_str;                       // in_str is 0 here
        ScanResult sr = scan_line(content, probe);
        int indent = depth - sr.leading_closers;
        if (indent < 0) indent = 0;
        out.append((size_t)indent * 4, ' ');
        out += content;
        out += "\n";

        depth += sr.delta;
        if (depth < 0) depth = 0;
        in_str = probe;                            // a multi-line string is carried over
    }

    // Guarantee a single trailing newline (end-of-file cleanup)
    while (out.size() >= 2 && out[out.size()-1] == '\n' && out[out.size()-2] == '\n') out.pop_back();
    if (out.empty() || out.back() != '\n') out += "\n";

    // -- MEANING-PRESERVATION GUARANTEE ------------------------------------------------------
    bool safe = same_tokens(src, out);
    if (ok) *ok = safe;
    return safe ? out : src;   // if not safe, return the ORIGINAL (no breakage)
}

int run_fmt(const std::vector<std::string>& files, bool check) {
    auto read_file = [](const std::string& p, std::string& out) -> bool {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
    };

    if (files.empty()) {   // stdin -> stdout
        std::stringstream ss; ss << std::cin.rdbuf();
        bool ok = false;
        std::string res = look_format_source(ss.str(), &ok);
        std::cout << res;
        if (!ok) { std::cerr << "lk fmt: warning — formatting skipped (semantics-preservation failed); input returned unchanged\n"; return 2; }
        return 0;
    }

    int rc = 0;
    for (const std::string& file : files) {
        std::string src;
        if (!read_file(file, src)) { std::cerr << "lk fmt: could not read: " << file << "\n"; rc = 2; continue; }
        bool ok = false;
        std::string res = look_format_source(src, &ok);
        if (!ok) { std::cerr << "lk fmt: " << file << " — formatting skipped (semantics-preservation failed, file unchanged)\n"; rc = 2; continue; }
        if (res == src) {
            // already formatted
        } else if (check) {
            std::cout << "unformatted: " << file << "\n";
            rc = 1;   // CI mode: there is an unformatted file
        } else {
            std::ofstream of(file, std::ios::binary | std::ios::trunc);
            of << res;
            std::cout << "formatted: " << file << "\n";
        }
    }
    return rc;
}

} // namespace look

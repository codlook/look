// Canonical path-under-root check — removes the SYMLINK and the upstream-decode assumption
// that a string-based traversal check cannot see. A single definition; cgi_main.cpp,
// fcgi_main.cpp (and conceptually file_stdlib.cpp assert_in_file_root) share the same pattern.
//
// A string check (looking for "../") is insufficient for two reasons:
//  1. symlink: the path doc/report.lk -> /other-tenant/config.lk has NO ".." but escapes doc.
//  2. upstream-decode: whether %2e or ".." reaches the guard differs between Apache/nginx/IIS.
// weakly_canonical both resolves the symlink and normalizes the path -> whatever the encoding/symlink,
// the decision is made on the CANONICAL path. Component-based comparison also closes the
// string-prefix bypass of "/srv/app" vs "/srv/app-evil".
#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace look {

inline bool path_under_root(const std::string& root, const std::string& cand) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path r = fs::weakly_canonical(fs::absolute(fs::path(root), ec), ec);
    if (ec) return false;
    fs::path c = fs::weakly_canonical(fs::absolute(fs::path(cand), ec), ec);
    if (ec) return false;
    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci)
        if (ci == c.end() || *ci != *ri) return false;   // component-based prefix
    return true;
}

} // namespace look

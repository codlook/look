// path_under_root vs string no_traversal — show the symlink gap empirically.
//   clang++ -std=c++17 -I../../include path_guard_test.cpp -o /tmp/pg && /tmp/pg
#include "look/path_guard.h"
#include <filesystem>
#include <cstdio>
#include <string>
namespace fs = std::filesystem;

static bool no_traversal(const std::string& s) {   // canli string kontrolu (kopya)
    return s.find("../") == std::string::npos &&
           s.find("..\\") == std::string::npos &&
           !(s.size() >= 2 && s.substr(s.size() - 2) == "..");
}

int main() {
    fs::remove_all("/tmp/dr"); fs::remove_all("/tmp/other");
    fs::create_directories("/tmp/dr/sub");
    fs::create_directories("/tmp/other");
    FILE* f = fopen("/tmp/dr/ok.lk", "w"); fputs("ok", f); fclose(f);
    f = fopen("/tmp/other/secret.lk", "w"); fputs("SECRET", f); fclose(f);
    fs::create_symlink("/tmp/other/secret.lk", "/tmp/dr/evil.lk");  // no ".." but it points outside

    struct Row { const char* label; std::string sn; bool str_expect; bool canon_expect; };
    Row rows[] = {
        {"control ok.lk",        "/tmp/dr/ok.lk",            true,  true },
        {"literal ../",          "/tmp/dr/../other/secret.lk", false, false},
        {"SYMLINK (critical)",     "/tmp/dr/evil.lk",          true,  false},  // <-- string MISSES it
        {"absolute outside",        "/tmp/other/secret.lk",     true,  false},
    };
    int fail = 0;
    printf("%-20s | string | canonical | expected\n", "payload");
    for (auto& r : rows) {
        bool s = no_traversal(r.sn);
        bool c = look::path_under_root("/tmp/dr", r.sn);
        bool ok = (s == r.str_expect) && (c == r.canon_expect);
        if (!ok) fail++;
        printf("%-20s |  %s   |  %s    | str=%d canon=%d  %s\n",
               r.label, s?"ALLOW":"DENY ", c?"ALLOW":"DENY ",
               r.str_expect, r.canon_expect, ok?"PASS":"FAIL");
    }
    printf(fail ? "\n%d FAIL\n" : "\nALL ROWS PASS — symlink: string ALLOWS, canonical DENIES\n", fail);
    return fail ? 1 : 0;
}

// Zip-Slip containment table test — look/zip_safe.h (open the seam). No network, NO miniz, NO real
// extract — only the security DECISION (is the entry extracted inside dest). Includes the sibling-prefix escape
// (the case installer.cpp's comment specifically defends against), with a positive control.
//   Build: g++ -std=c++17 -Iinclude tests/installer_zipslip_test.cpp -o /tmp/izt && /tmp/izt
#include "look/zip_safe.h"
#include <cstdio>
#include <string>
#include <filesystem>
using namespace look;
namespace fs = std::filesystem;

static int fails = 0, ran = 0;
// filename · expected (is it inside dest)
static void chk(const char* filename, bool want) {
    ran++;
    fs::path dest = "/tmp/zs/user/repo";   // need not exist (weakly_canonical is lexical)
    bool got = zip_entry_inside_dest(filename, dest);
    bool ok = (got == want);
    printf("  %-24s -> inside=%d (want %d)  %s\n", filename, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("Zip-Slip containment tablosu (dest=/tmp/zs/user/repo):\n");
    // INSIDE — legitimate
    chk("good.lk",        true);
    chk("sub/ok.lk",      true);
    chk("a/../b.lk",      true);    // normalize → repo/b.lk, still inside
    // OUTSIDE — must be rejected
    chk("../../evil.lk",  false);   // classic traversal
    chk("../repo-evil/x", false);   // SIBLING-PREFIX escape (repo-evil starts with "repo" but is outside)
    chk("/etc/passwd",    false);   // absolute path (dest/'/etc' → /etc)
    chk("a/../../out.lk", false);   // escape from a subdirectory
    chk("../../../../tmp/x", false);// derin traversal

    // ZIP bomb decision (zip_size_problem): empty = accept
    printf("ZIP size limit:\n");
    auto zchk = [&](const char* name, const std::vector<uint64_t>& sizes, bool want_ok) {
        ran++;
        std::string why = zip_size_problem(sizes);
        bool ok = (why.empty() == want_ok);
        printf("  %-34s -> %s  %s\n", name, why.empty() ? "accepted" : why.c_str(), ok ? "OK" : "FAIL");
        if (!ok) fails++;
    };
    const uint64_t MB = 1024 * 1024;
    zchk("ordinary package (3 files)",      {1200, 40 * 1024, 9}, true);
    zchk("exactly 256 MB",                  {128 * MB, 128 * MB}, true);
    zchk("256 MB + 1 byte",                 {128 * MB, 128 * MB, 1}, false);
    zchk("one 4 GB entry (bomb)",           {4096 * MB}, false);
    zchk("sizes that overflow uint64",      {UINT64_MAX, 2}, false);
    zchk("20000 empty entries",             std::vector<uint64_t>(20000, 0), true);
    zchk("20001 entries",                   std::vector<uint64_t>(20001, 0), false);

    const int EXPECTED = 15;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (sibling-prefix kaçışı dahil, Zip-Slip kilitli)\n", fails);
    return fails ? 1 : 0;
}

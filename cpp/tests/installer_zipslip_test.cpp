// Zip-Slip containment tablo testi — look/zip_safe.h (dikişi aç). Ağsız, miniz YOK, gerçek
// extract YOK — yalnız güvenlik-KARARI (entry dest içine mi açılır). Sibling-prefix kaçışı
// (installer.cpp yorumunun özellikle savunduğu vaka) dahil, pozitif-kontrollü.
//   Build: g++ -std=c++17 -Iinclude tests/installer_zipslip_test.cpp -o /tmp/izt && /tmp/izt
#include "look/zip_safe.h"
#include <cstdio>
#include <string>
#include <filesystem>
using namespace look;
namespace fs = std::filesystem;

static int fails = 0, ran = 0;
// filename · beklenen (dest içinde mi)
static void chk(const char* filename, bool want) {
    ran++;
    fs::path dest = "/tmp/zs/user/repo";   // var olması gerekmez (weakly_canonical lexical)
    bool got = zip_entry_inside_dest(filename, dest);
    bool ok = (got == want);
    printf("  %-24s -> inside=%d (want %d)  %s\n", filename, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("Zip-Slip containment tablosu (dest=/tmp/zs/user/repo):\n");
    // İÇERİDE — meşru
    chk("good.lk",        true);
    chk("sub/ok.lk",      true);
    chk("a/../b.lk",      true);    // normalize → repo/b.lk, hâlâ içeride
    // DIŞARIDA — reddedilmeli
    chk("../../evil.lk",  false);   // klasik traversal
    chk("../repo-evil/x", false);   // SIBLING-PREFIX kaçışı (repo-evil, "repo" ile başlar ama dışında)
    chk("/etc/passwd",    false);   // mutlak yol (dest/'/etc' → /etc)
    chk("a/../../out.lk", false);   // alt-dizinden kaçış
    chk("../../../../tmp/x", false);// derin traversal

    const int EXPECTED = 8;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (sibling-prefix kaçışı dahil, Zip-Slip kilitli)\n", fails);
    return fails ? 1 : 0;
}

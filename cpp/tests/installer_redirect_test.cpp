// installer yönlendirme güvenlik-kararı tablo testi — look/url_safe.h (dikişi aç). Ağsız,
// gerçek HTTP/3xx YOK — yalnız "bu redirect'e izin var mı" kararı. Şema-düşürme + host-kaçışı
// (evilgithub.com / github.com.evil.com) dahil, pozitif-kontrollü.
//   Build: g++ -std=c++17 -Iinclude tests/installer_redirect_test.cpp -o /tmp/irt && /tmp/irt
#include "look/url_safe.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
static void chk(const char* url, bool want) {
    ran++;
    std::string err;
    bool got = redirect_allowed(url, err);
    bool ok = (got == want);
    printf("  %-42s -> %d (want %d)  %s\n", url, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("Redirect güvenlik-karar tablosu:\n");
    // İZİN — meşru GitHub zinciri
    chk("https://codeload.github.com/u/r/zip", true);
    chk("https://api.github.com/repos/u/r",    true);
    chk("https://github.com/u/r",              true);
    chk("https://objects.githubusercontent.com/x", true);
    // RED — şema düşürme
    chk("http://github.com/u/r",               false);   // 48. bug: https→http
    chk("ftp://github.com/x",                  false);
    // RED — host kaçışı
    chk("https://evil.com/x",                  false);
    chk("https://evilgithub.com/x",            false);    // öneksiz (".github.com" değil)
    chk("https://github.com.evil.com/x",       false);    // sonek yanlış-alan
    chk("https://api.github.com.attacker.net/x", false);

    const int EXPECTED = 10;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (şema-düşürme + host-kaçışı kilitli)\n", fails);
    return fails ? 1 : 0;
}

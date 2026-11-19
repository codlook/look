// installer redirect security-decision table test — look/url_safe.h (open the seam). No network,
// NO real HTTP/3xx — only the "is this redirect allowed" decision. Includes scheme downgrade + host escape
// (evilgithub.com / github.com.evil.com), with a positive control.
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
    printf("Redirect security-decision table:\n");
    // ALLOW — legitimate GitHub chain
    chk("https://codeload.github.com/u/r/zip", true);
    chk("https://api.github.com/repos/u/r",    true);
    chk("https://github.com/u/r",              true);
    chk("https://objects.githubusercontent.com/x", true);
    // REJECT — scheme downgrade
    chk("http://github.com/u/r",               false);   // 48. bug: https→http
    chk("ftp://github.com/x",                  false);
    // REJECT — host escape
    chk("https://evil.com/x",                  false);
    chk("https://evilgithub.com/x",            false);    // no prefix (not ".github.com")
    chk("https://github.com.evil.com/x",       false);    // suffix wrong-domain
    chk("https://api.github.com.attacker.net/x", false);

    const int EXPECTED = 10;
    if (ran != EXPECTED) { printf("\nFAIL: %d cases expected, %d ran\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nALL CASES PASSED (scheme downgrade + host escape locked in)\n", fails);
    return fails ? 1 : 0;
}

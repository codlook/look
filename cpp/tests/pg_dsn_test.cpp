// PostgreSQL DSN TLS-decision table test — look/db_dsn.h (open the seam). No network, NO handshake.
// Locks the verify-ca silent-plaintext regression (558d735) WITH POSITIVE CONTROLS.
//   Build: g++ -std=c++17 -Iinclude tests/pg_dsn_test.cpp -o /tmp/pgdsn && /tmp/pgdsn
#include "look/db_dsn.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// query · scheme_secure · expected tls · expected verify
static void chk(const char* q, bool sec, bool wtls, bool wver) {
    ran++;
    bool tls = false, ver = false;
    pg_resolve_tls(q, sec, tls, ver);
    bool ok = (tls == wtls) && (tls == false || ver == wver);  // verify is irrelevant when tls=false
    printf("  %-26s sec=%d -> tls=%d ver=%d  %s\n", (std::string("\"")+q+"\"").c_str(),
           sec, tls, ver, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("PG DSN TLS-karar tablosu:\n");
    // defaults
    chk("", true,  true,  true);    // postgresqls:// without query → TLS + verify (safe default)
    chk("", false, false, true);    // postgres:// without query → no TLS
    // insecure / require → encrypted but unverified
    chk("tls=insecure",     false, true, false);
    chk("ssl=insecure",     false, true, false);
    chk("sslmode=require",  false, true, false);
    // verify families → TLS + verification
    chk("tls=verify",          false, true, true);
    chk("ssl=verify",          false, true, true);
    chk("sslmode=verify-full", false, true, true);
    chk("tls=1",               false, true, true);
    chk("tls=true",            false, true, true);
    // CRITICAL REGRESSION (558d735): verify-ca matched no branch and silently fell to plaintext.
    // postgres:// (sec=false) + verify-ca → must be tls=true, verify=true. If someone one day
    // deletes this branch tls=false returns → this line goes RED (positive control).
    chk("sslmode=verify-ca", false, true, true);
    // unknown sslmode → no branch → the scheme default is preserved
    chk("sslmode=disable",   false, false, true);
    chk("foo=bar",           true,  true,  true);

    const int EXPECTED = 13;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (verify-ca dahil, sessiz-plaintext regresyonu kilitli)\n", fails);
    return fails ? 1 : 0;
}

// MySQL + Redis DSN TLS-karar tablo testi — look/db_dsn.h (dikişi aç). Ağsız, handshake YOK.
// PG'den FARK: verify=false VARSAYILAN (DB cert'leri self-signed; kablo-şifreleme amaç).
// Bu test MEVCUT kararı POZİTİF-KONTROLLÜ kilitler (varsayılan bir gün değişirse RED).
//   Build: g++ -std=c++17 -Iinclude tests/mysql_redis_dsn_test.cpp -o /tmp/mrdsn && /tmp/mrdsn
#include "look/db_dsn.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
enum Drv { MY, RD };
static void chk(Drv d, const char* q, bool sec, bool wtls, bool wver) {
    ran++;
    bool tls = false, ver = false;
    if (d == MY) mysql_resolve_tls(q, sec, tls, ver);
    else         redis_resolve_tls(q, sec, tls, ver);
    bool ok = (tls == wtls) && (ver == wver);
    printf("  %-5s %-24s sec=%d -> tls=%d ver=%d %s\n", d==MY?"mysql":"redis",
           (std::string("\"")+q+"\"").c_str(), sec, tls, ver, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("MySQL/Redis DSN TLS-karar tablosu (verify=false VARSAYILAN):\n");
    // ── MySQL ──
    chk(MY, "",            false, false, false); // mysql:// düz → TLS yok
    chk(MY, "",            true,  true,  false); // mysqls:// → şifreli AMA doğrulamasız (varsayılan)
    chk(MY, "tls=1",       false, true,  false); // yalnız şifreleme
    chk(MY, "tls=true",    false, true,  false);
    chk(MY, "ssl=1",       false, true,  false); // herhangi ssl= → şifreleme
    chk(MY, "tls=verify",  false, true,  true);  // CA+hostname doğrulama
    chk(MY, "ssl=verify",  false, true,  true);
    chk(MY, "ssl=verify_identity", false, true, true); // MySQL'e özgü
    chk(MY, "foo=bar",     true,  true,  false); // bilinmeyen query → şema varsayılanı
    chk(MY, "foo=bar",     false, false, false);
    // ── Redis ──
    chk(RD, "",            false, false, false); // redis:// düz
    chk(RD, "",            true,  true,  false); // rediss:// → şifreli, doğrulamasız
    chk(RD, "tls=1",       false, true,  false);
    chk(RD, "ssl=1",       false, true,  false);
    chk(RD, "tls=verify",  false, true,  true);
    chk(RD, "ssl=verify",  false, true,  true);
    chk(RD, "foo=bar",     false, false, false);

    const int EXPECTED = 17;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (verify=false varsayılanı + verify açık-istek kilitli)\n", fails);
    return fails ? 1 : 0;
}

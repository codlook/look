// MySQL + Redis DSN TLS-karar tablo testi — look/db_dsn.h (dikişi aç). Ağsız, handshake YOK.
// GÜVENLİ-VARSAYILAN (2026-08-11 flip): TLS açıkken verify=true VARSAYILAN (http/PG ile hizalı);
// self-signed DB için AÇIK opt-out ?tls=insecure. Bu test yeni kararı + kaçış-kapağını
// POZİTİF-KONTROLLÜ kilitler (biri varsayılanı geri çevirirse veya insecure dalını silerse RED).
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
    printf("MySQL/Redis DSN TLS-karar tablosu (verify=true GÜVENLİ-VARSAYILAN + ?tls=insecure opt-out):\n");
    // ── MySQL ──
    chk(MY, "",            false, false, true);  // mysql:// düz → TLS yok (verify önemsiz, true)
    chk(MY, "",            true,  true,  true);  // mysqls:// → şifreli + DOĞRULANMIŞ (güvenli varsayılan)
    chk(MY, "tls=1",       false, true,  true);  // TLS aç → verify güvenli-varsayılan
    chk(MY, "tls=true",    false, true,  true);
    chk(MY, "ssl=1",       false, true,  true);  // herhangi ssl= → TLS + verify
    chk(MY, "tls=insecure",false, true,  false); // AÇIK opt-out → şifreli AMA doğrulamasız
    chk(MY, "ssl=insecure",false, true,  false);
    chk(MY, "tls=verify",  false, true,  true);  // açık verify (varsayılanla aynı)
    chk(MY, "ssl=verify",  false, true,  true);
    chk(MY, "ssl=verify_identity", false, true, true); // MySQL'e özgü
    chk(MY, "foo=bar",     true,  true,  true);  // bilinmeyen query → şema + güvenli-varsayılan
    chk(MY, "foo=bar",     false, false, true);
    // ── Redis ──
    chk(RD, "",            false, false, true);  // redis:// düz
    chk(RD, "",            true,  true,  true);  // rediss:// → şifreli + DOĞRULANMIŞ
    chk(RD, "tls=1",       false, true,  true);
    chk(RD, "ssl=1",       false, true,  true);
    chk(RD, "tls=insecure",false, true,  false); // AÇIK opt-out
    chk(RD, "ssl=insecure",false, true,  false);
    chk(RD, "tls=verify",  false, true,  true);
    chk(RD, "ssl=verify",  false, true,  true);
    chk(RD, "foo=bar",     false, false, true);

    const int EXPECTED = 21;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (verify=true güvenli-varsayılan + ?tls=insecure opt-out kilitli)\n", fails);
    return fails ? 1 : 0;
}

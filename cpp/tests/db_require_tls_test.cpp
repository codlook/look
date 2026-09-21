// LOOK_DB_REQUIRE_TLS karar tablosu — look/db_dsn.h db_refuse_plaintext (dikişi aç).
// Ağsız/handshake-siz. Politika: uzak host'a düz metin bağlantıyı REDDET; loopback/unix
// HER ZAMAN serbest; ?insecure_plaintext=1 açık opt-out. String-bazlı loopback tespiti
// (literal adres — DNS-çözüm yok, dokümante). Pozitif-kontrollü: biri loopback dalını
// veya opt-out'u silerse RED.
//   Build: g++ -std=c++17 -Iinclude tests/db_require_tls_test.cpp -o /tmp/drt && /tmp/drt
#include "look/db_dsn.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// refuse: require_tls, encrypted, host, insecure_plaintext -> beklenen ret?
static void chk(bool req, bool enc, const char* host, bool ipt, bool want) {
    ran++;
    bool got = db_refuse_plaintext(req, enc, host, ipt);
    bool ok = (got == want);
    printf("  req=%d enc=%d ipt=%d host=%-18s -> refuse=%d %s\n",
           req, enc, ipt, (std::string("\"")+host+"\"").c_str(), got, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}
static void chk_local(const char* host, bool want) {
    ran++;
    bool got = db_host_is_local(host);
    bool ok = (got == want);
    printf("  is_local %-20s -> %d %s\n", (std::string("\"")+host+"\"").c_str(), got, ok?"OK":"FAIL");
    if (!ok) fails++;
}
static void chk_opt(const char* q, bool want) {
    ran++;
    bool got = db_insecure_plaintext_opt(q);
    bool ok = (got == want);
    printf("  opt %-28s -> %d %s\n", (std::string("\"")+q+"\"").c_str(), got, ok?"OK":"FAIL");
    if (!ok) fails++;
}

int main() {
    printf("LOOK_DB_REQUIRE_TLS refuse-plaintext karar tablosu:\n");
    // Politika KAPALI → asla reddetme (davranış-değiştirmez, geriye-uyum)
    chk(false, false, "db.remote.com", false, false);
    chk(false, false, "10.0.0.9",      false, false);
    // Politika AÇIK + şifreli → asla reddetme
    chk(true,  true,  "db.remote.com", false, false);
    // Politika AÇIK + düz metin + LOOPBACK/unix → serbest (asıl endişenin cevabı)
    chk(true,  false, "127.0.0.1",     false, false);
    chk(true,  false, "localhost",     false, false);
    chk(true,  false, "::1",           false, false);
    chk(true,  false, "[::1]",         false, false);
    chk(true,  false, "",              false, false);   // unix soket / belirtilmemiş
    chk(true,  false, "/var/run/mysqld/mysqld.sock", false, false);
    chk(true,  false, "127.0.0.53",    false, false);   // 127.0.0.0/8
    // Politika AÇIK + düz metin + UZAK → REDDET (asıl kapatılan tehlike)
    chk(true,  false, "db.remote.com", false, true);
    chk(true,  false, "10.0.0.9",      false, true);
    chk(true,  false, "192.168.1.20",  false, true);
    chk(true,  false, "203.0.113.7",   false, true);
    // Politika AÇIK + düz metin + UZAK + ?insecure_plaintext=1 → serbest (bilinçli opt-out)
    chk(true,  false, "db.remote.com", true,  false);
    // ── loopback tespiti (string-bazlı, literal) ──
    chk_local("127.0.0.1", true); chk_local("localhost", true); chk_local("::1", true);
    chk_local("[::1]", true); chk_local("", true); chk_local("/tmp/x.sock", true);
    chk_local("127.5.6.7", true);
    chk_local("db.local", false);   // hostname (127.0.0.1'e çözülse bile UZAK sayılır — dokümante)
    chk_local("10.0.0.1", false); chk_local("192.168.0.1", false);
    // ── ?insecure_plaintext opt-out parse ──
    chk_opt("insecure_plaintext=1", true);
    chk_opt("insecure_plaintext=true", true);
    chk_opt("tls=insecure", false);       // TLS-insecure BAŞKA şey (şifreli-doğrulamasız)
    chk_opt("foo=bar", false);
    chk_opt("", false);

    printf("\n%d/%d PASS%s\n", ran - fails, ran, fails ? "  — FAIL VAR" : "");
    return fails ? 1 : 0;
}

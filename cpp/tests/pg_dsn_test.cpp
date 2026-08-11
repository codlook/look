// PostgreSQL DSN TLS-karar tablo testi — look/db_dsn.h (dikişi aç). Ağsız, handshake YOK.
// verify-ca sessiz-plaintext regresyonunu (558d735) POZİTİF-KONTROLLÜ kilitler.
//   Build: g++ -std=c++17 -Iinclude tests/pg_dsn_test.cpp -o /tmp/pgdsn && /tmp/pgdsn
#include "look/db_dsn.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// query · scheme_secure · beklenen tls · beklenen verify
static void chk(const char* q, bool sec, bool wtls, bool wver) {
    ran++;
    bool tls = false, ver = false;
    pg_resolve_tls(q, sec, tls, ver);
    bool ok = (tls == wtls) && (tls == false || ver == wver);  // tls=false ise verify önemsiz
    printf("  %-26s sec=%d -> tls=%d ver=%d  %s\n", (std::string("\"")+q+"\"").c_str(),
           sec, tls, ver, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("PG DSN TLS-karar tablosu:\n");
    // varsayılanlar
    chk("", true,  true,  true);    // postgresqls:// query'siz → TLS + verify (güvenli varsayılan)
    chk("", false, false, true);    // postgres:// query'siz → TLS yok
    // insecure / require → şifreli ama doğrulamasız
    chk("tls=insecure",     false, true, false);
    chk("ssl=insecure",     false, true, false);
    chk("sslmode=require",  false, true, false);
    // verify aileleri → TLS + doğrulama
    chk("tls=verify",          false, true, true);
    chk("ssl=verify",          false, true, true);
    chk("sslmode=verify-full", false, true, true);
    chk("tls=1",               false, true, true);
    chk("tls=true",            false, true, true);
    // KRİTİK REGRESYON (558d735): verify-ca hiçbir dala uymayıp sessizce plaintext'e düşüyordu.
    // postgres:// (sec=false) + verify-ca → tls=true, verify=true olmalı. Bir gün biri bu dalı
    // silerse tls=false döner → bu satır KIRMIZI (pozitif kontrol).
    chk("sslmode=verify-ca", false, true, true);
    // bilinmeyen sslmode → hiçbir dal → şema varsayılanı korunur
    chk("sslmode=disable",   false, false, true);
    chk("foo=bar",           true,  true,  true);

    const int EXPECTED = 13;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (verify-ca dahil, sessiz-plaintext regresyonu kilitli)\n", fails);
    return fails ? 1 : 0;
}

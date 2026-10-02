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

// ── Havuz-açılış DB-TLS uyarısı (db_tls_warning, 2026-09-30) ──
// Uyarı GERÇEK karara eşit olmalı. Beklenen: 'N' uyarı yok · 'U' UNENCRYPTED · 'I' NOT VERIFIED.
static char wkind(const std::string& w) {
    if (w.empty()) return 'N';
    if (w.find("UNENCRYPTED") != std::string::npos) return 'U';
    if (w.find("NOT VERIFIED") != std::string::npos) return 'I';
    return '?';
}
// Pozitif kontrol: ESKİ çıplak-substring mantığının birebir kopyası (web_stdlib, abb5e2e).
// Bug vakalarında eski mantık YANLIŞ sonuç vermeli — vermiyorsa tablo ayrımcı değildir.
static char legacy_kind(const std::string& dsn) {
    std::string sch = dsn.substr(0, dsn.find(':'));
    bool has_tls = dsn.find("tls=") != std::string::npos || dsn.find("ssl=") != std::string::npos;
    bool insecure = dsn.find("tls=insecure") != std::string::npos || dsn.find("ssl=insecure") != std::string::npos;
    bool secure_sch = (sch == "mysqls" || sch == "mariadbs" || sch == "rediss");
    bool encrypted = secure_sch || (has_tls && (sch == "mysql" || sch == "mariadb" || sch == "redis"));
    if (sch == "postgres" || sch == "postgresql" || sch == "postgresqls") {
        bool pe = (sch == "postgresqls") || (has_tls && (dsn.find("tls=") != std::string::npos || dsn.find("sslmode=") != std::string::npos));
        bool pi = insecure || dsn.find("sslmode=require") != std::string::npos;
        return !pe ? 'U' : pi ? 'I' : 'N';
    }
    if (sch != "mysql" && sch != "mariadb" && sch != "redis" && !secure_sch) return 'N';
    if (!encrypted) return 'U';
    return insecure ? 'I' : 'N';
}
static void chkw(const char* dsn, char want, bool legacy_wrong = false) {
    ran++;
    char got = wkind(db_tls_warning(dsn));
    char old = legacy_kind(dsn);
    bool ok = got == want && (!legacy_wrong || old != want);
    printf("  warn  %-48s -> %c (legacy %c)%s %s\n", dsn, got, old,
           legacy_wrong ? " [bug]" : "", ok ? "OK" : "FAIL");
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
    // ── parser adversarial turu (2026-09-21): sınır-duyarlı param eşleme ──
    // #1: ssl/tls FALSEY değeri TLS'i ZORLAMAZ (eski çıplak-substring "ssl=" TLS'i açıyordu).
    chk(MY, "ssl=false",   false, false, true);
    chk(MY, "ssl=0",       false, false, true);
    chk(MY, "ssl=off",     false, false, true);
    chk(MY, "ssl=disable", false, false, true);
    chk(MY, "tls=false",   false, false, true);
    chk(MY, "sslmode=disable", false, false, true); // "ssl=" substring'i DEĞİL → TLS yok
    // #4: değer-içi enjeksiyon (opt=tls=insecure) verify'ı DÜŞÜRMEZ — "tls" param sınırında değil.
    chk(MY, "dbname=x&opt=tls=insecure", false, false, true);
    chk(MY, "name=tls=verify_notreal",   false, false, true);
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
    chk(RD, "ssl=false",   false, false, true);  // #1: falsey → TLS yok

    // ── Uyarı = karar (db_tls_warning) ──
    chkw("mysql://u:p@db:3306/app",                        'U');
    chkw("mysqls://u:p@db:3306/app",                       'N');
    chkw("mysql://u:p@db:3306/app?tls=1",                  'N');
    chkw("mysql://u:p@db:3306/app?tls=insecure",           'I');
    chkw("mariadbs://u:p@db/app?ssl=insecure",             'I');
    chkw("redis://:p@r:6379",                              'U');
    chkw("rediss://:p@r:6379",                             'N');
    chkw("redis://r:6379?tls=insecure",                    'I');
    chkw("postgres://u:p@pg/app",                          'U');
    chkw("postgresqls://u:p@pg/app",                       'N');
    chkw("postgres://u:p@pg/app?sslmode=require",          'I', true); // eski 'U' derdi (şifreli ama doğrulamasız)
    chkw("postgres://u:p@pg/app?tls=verify",               'N');
    chkw("sqlite://app.db",                                'N');
    // bug vakaları — eski substring mantığı YANLIŞ (pozitif kontrol)
    chkw("mysql://u:tls=1@db:3306/app",                    'U', true); // (a) parolada tls=
    chkw("postgres://u:tls=1@pg/app",                      'U', true); // (a) PG, canlı doğrulandı
    chkw("redis://:ssl=1@r:6379",                          'U', true); // (a) redis parola
    chkw("mysql://u:p@db:3306/app?ssl=false",              'U', true); // (b) ssl=false = TLS yok
    chkw("redis://r:6379?tls=0",                           'U', true); // (b) redis falsey
    chkw("mysql://u:p@db:3306/app?x=tls=insecure",         'U', true); // (c) değer-içi, plaintext
    chkw("mysqls://u:p@db:3306/app?x=tls=insecure",        'N', true); // (c) mysqls doğrulanmış
    chkw("postgres://u:p@pg/app?x=tls=insecure",           'U', true); // (c) PG, canlı doğrulandı
    chkw("postgres://u:p@pg/app?sslmode=verify-ca",        'N', true); // 558d735 sınıfı: eski 'U' derdi
    chkw("mysql://u:p?tls=insecure@db:3306/app",           'U', true); // userinfo'daki '?' sorgu değil

    // Redis bölme sırası: önce son '@', sonra '?'. Eski sıra ('?' önce) parolasında '?'
    // olan URL'de host'u ":pa", sorguyu "ss@r:6379" çıkarıyordu.
    {
        struct S { const char* in; const char* ui; const char* hp; const char* q; };
        const S cases[] = {
            {":pa?ss@r:6379/2",            ":pa?ss", "r:6379/2", ""},
            {":pa?ss@r:6379/2?tls=verify", ":pa?ss", "r:6379/2", "tls=verify"},
            {":p@r:6379?tls=1",            ":p",     "r:6379",   "tls=1"},
            {"r:6379",                     "",       "r:6379",   ""},
            {"user:p:q@r",                 "user:p:q", "r",      ""},
        };
        for (const S& c : cases) {
            std::string ui, hp, q;
            look::redis_split_url(c.in, ui, hp, q);
            bool ok = ui == c.ui && hp == c.hp && q == c.q;
            printf("  %s redis_split_url(%s) -> [%s] [%s] [%s]\n", ok ? "PASS" : "FAIL", c.in, ui.c_str(), hp.c_str(), q.c_str());
            if (!ok) ++fails;
            ++ran;
        }
    }
    chkw("redis://:pa?tls=insecure@r:6379",                'U', true); // parolada '?': sorgu değil
    chkw("rediss://:pa?ss@r:6379",                         'N');

    const int EXPECTED = 60;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (verify=true güvenli-varsayılan + ?tls=insecure opt-out kilitli)\n", fails);
    return fails ? 1 : 0;
}

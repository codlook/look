// MySQL + Redis DSN TLS-decision table test — look/db_dsn.h (open the seam). No network, NO handshake.
// SAFE DEFAULT (2026-08-11 flip): when TLS is on verify=true is the DEFAULT (aligned with http/PG);
// for a self-signed DB an EXPLICIT opt-out ?tls=insecure. This test locks the new decision + the escape hatch
// WITH POSITIVE CONTROLS (RED if someone reverts the default or deletes the insecure branch).
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

// ── Pool-open DB-TLS warning (db_tls_warning, 2026-09-30) ──
// The warning must equal the REAL decision. Expected: 'N' no warning · 'U' UNENCRYPTED · 'I' NOT VERIFIED.
static char wkind(const std::string& w) {
    if (w.empty()) return 'N';
    if (w.find("UNENCRYPTED") != std::string::npos) return 'U';
    if (w.find("NOT VERIFIED") != std::string::npos) return 'I';
    return '?';
}
// Positive control: an exact copy of the OLD bare-substring logic (web_stdlib, abb5e2e).
// On the bug cases the old logic must give a WRONG result — if not, the table is not discriminating.
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
    printf("MySQL/Redis DSN TLS decision table (verify=true SAFE DEFAULT + ?tls=insecure opt-out):\n");
    // ── MySQL ──
    chk(MY, "",            false, false, true);  // mysql:// plain → no TLS (verify irrelevant, true)
    chk(MY, "",            true,  true,  true);  // mysqls:// → encrypted + VERIFIED (safe default)
    chk(MY, "tls=1",       false, true,  true);  // enable TLS → verify safe default
    chk(MY, "tls=true",    false, true,  true);
    chk(MY, "ssl=1",       false, true,  true);  // any ssl= → TLS + verify
    chk(MY, "tls=insecure",false, true,  false); // EXPLICIT opt-out → encrypted BUT unverified
    chk(MY, "ssl=insecure",false, true,  false);
    chk(MY, "tls=verify",  false, true,  true);  // explicit verify (same as the default)
    chk(MY, "ssl=verify",  false, true,  true);
    chk(MY, "ssl=verify_identity", false, true, true); // MySQL-specific
    chk(MY, "foo=bar",     true,  true,  true);  // unknown query → scheme + safe default
    chk(MY, "foo=bar",     false, false, true);
    // ── parser adversarial round (2026-09-21): boundary-aware param matching ──
    // #1: a FALSEY ssl/tls value does NOT FORCE TLS (the old bare substring "ssl=" turned TLS on).
    chk(MY, "ssl=false",   false, false, true);
    chk(MY, "ssl=0",       false, false, true);
    chk(MY, "ssl=off",     false, false, true);
    chk(MY, "ssl=disable", false, false, true);
    chk(MY, "tls=false",   false, false, true);
    chk(MY, "sslmode=disable", false, false, true); // NOT the "ssl=" substring → no TLS
    // #4: injection inside a value (opt=tls=insecure) does NOT drop verify — "tls" is not at a param boundary.
    chk(MY, "dbname=x&opt=tls=insecure", false, false, true);
    chk(MY, "name=tls=verify_notreal",   false, false, true);
    // ── Redis ──
    chk(RD, "",            false, false, true);  // redis:// plain
    chk(RD, "",            true,  true,  true);  // rediss:// → encrypted + VERIFIED
    chk(RD, "tls=1",       false, true,  true);
    chk(RD, "ssl=1",       false, true,  true);
    chk(RD, "tls=insecure",false, true,  false); // EXPLICIT opt-out
    chk(RD, "ssl=insecure",false, true,  false);
    chk(RD, "tls=verify",  false, true,  true);
    chk(RD, "ssl=verify",  false, true,  true);
    chk(RD, "foo=bar",     false, false, true);
    chk(RD, "ssl=false",   false, false, true);  // #1: falsey → no TLS

    // ── Warning = decision (db_tls_warning) ──
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
    chkw("postgres://u:p@pg/app?sslmode=require",          'I', true); // the old one would say 'U' (encrypted but unverified)
    chkw("postgres://u:p@pg/app?tls=verify",               'N');
    chkw("sqlite://app.db",                                'N');
    // bug cases — the old substring logic is WRONG (positive control)
    chkw("mysql://u:tls=1@db:3306/app",                    'U', true); // (a) tls= in the password
    chkw("postgres://u:tls=1@pg/app",                      'U', true); // (a) PG, verified live
    chkw("redis://:ssl=1@r:6379",                          'U', true); // (a) redis password
    chkw("mysql://u:p@db:3306/app?ssl=false",              'U', true); // (b) ssl=false = no TLS
    chkw("redis://r:6379?tls=0",                           'U', true); // (b) redis falsey
    chkw("mysql://u:p@db:3306/app?x=tls=insecure",         'U', true); // (c) inside a value, plaintext
    chkw("mysqls://u:p@db:3306/app?x=tls=insecure",        'N', true); // (c) mysqls verified
    chkw("postgres://u:p@pg/app?x=tls=insecure",           'U', true); // (c) PG, verified live
    chkw("postgres://u:p@pg/app?sslmode=verify-ca",        'N', true); // 558d735 class: the old one would say 'U'
    chkw("mysql://u:p?tls=insecure@db:3306/app",           'U', true); // a '?' in the userinfo is not a query
    // DSN without a path: the connection (parse_dsn_part, Go net/url) reads the query → the warning must read it too.
    chkw("mysql://u:p@db:3306?tls=insecure",               'I');
    chkw("mysql://u:p@db:3306?tls=1",                      'N');
    chkw("postgres://u:p@pg:5432?sslmode=require",         'I');

    // Redis split order: first the last '@', then '?'. The old order ('?' first) on a URL with a '?' in the password
    // produced host ":pa" and query "ss@r:6379".
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
    chkw("redis://:pa?tls=insecure@r:6379",                'U', true); // '?' in the password: not a query
    chkw("rediss://:pa?ss@r:6379",                         'N');

    const int EXPECTED = 63;
    if (ran != EXPECTED) { printf("\nFAIL: %d cases expected, %d ran\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nALL CASES PASSED (verify=true safe default + ?tls=insecure opt-out locked in)\n", fails);
    return fails ? 1 : 0;
}

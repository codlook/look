// Decision table of LOOK_DB_REQUIRE_TLS -- look/db_dsn.h db_refuse_plaintext (seam opened).
// No network / no handshake. Policy: REFUSE a plaintext connection to a remote host; loopback/unix
// is ALWAYS allowed; ?insecure_plaintext=1 is an explicit opt-out. String-based loopback detection
// (literal address -- no DNS resolution, documented). With positive controls: RED if someone deletes the loopback branch
// or the opt-out.
//   Build: g++ -std=c++17 -Iinclude tests/db_require_tls_test.cpp -o /tmp/drt && /tmp/drt
#include "look/db_dsn.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// refuse: require_tls, encrypted, host, insecure_plaintext -> expected return?
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
// Log redaction: the expected format AND no part of the password in the output (secret="" -> no check).
static void chk_redact(const char* dsn, const char* want, const char* secret) {
    ran++;
    std::string got = db_dsn_redact(dsn);
    bool ok = (got == want) && (secret[0] == '\0' || got.find(secret) == std::string::npos);
    printf("  redact %-44s -> %-40s %s\n", dsn, got.c_str(), ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("LOOK_DB_REQUIRE_TLS refuse-plaintext karar tablosu:\n");
    // Policy OFF -> never refuse (does not change behavior, backward compatible)
    chk(false, false, "db.remote.com", false, false);
    chk(false, false, "10.0.0.9",      false, false);
    // Policy ON + encrypted -> never refuse
    chk(true,  true,  "db.remote.com", false, false);
    // Policy ON + plaintext + LOOPBACK/unix -> allowed (the answer to the original concern)
    chk(true,  false, "127.0.0.1",     false, false);
    chk(true,  false, "localhost",     false, false);
    chk(true,  false, "::1",           false, false);
    chk(true,  false, "[::1]",         false, false);
    chk(true,  false, "",              false, false);   // unix socket / unspecified
    chk(true,  false, "/var/run/mysqld/mysqld.sock", false, false);
    chk(true,  false, "127.0.0.53",    false, false);   // 127.0.0.0/8
    // Policy ON + plaintext + REMOTE -> REFUSE (the danger actually being closed)
    chk(true,  false, "db.remote.com", false, true);
    chk(true,  false, "10.0.0.9",      false, true);
    chk(true,  false, "192.168.1.20",  false, true);
    chk(true,  false, "203.0.113.7",   false, true);
    // Policy ON + plaintext + REMOTE + ?insecure_plaintext=1 -> allowed (deliberate opt-out)
    chk(true,  false, "db.remote.com", true,  false);
    // ── loopback detection (string-based, literal) ──
    chk_local("127.0.0.1", true); chk_local("localhost", true); chk_local("::1", true);
    chk_local("[::1]", true); chk_local("", true); chk_local("/tmp/x.sock", true);
    chk_local("127.5.6.7", true);
    chk_local("db.local", false);   // hostname (counts as REMOTE even if it resolves to 127.0.0.1 -- documented)
    chk_local("10.0.0.1", false); chk_local("192.168.0.1", false);
    // ── parser adversarial round (2026-09-21) ──
    // #3: a hostname with a "127." prefix BUT containing letters is REMOTE (the old prefix match counted it as local and skipped REQUIRE_TLS)
    chk_local("127.evil.com", false);
    chk_local("127.0.0.1.nip.io", false);
    chk_local("127x", false);
    chk_local("1270.0.0.1", false);     // does not start with the "127." prefix
    // #3 end to end: plaintext to 127.evil.com is counted as REMOTE and REFUSED
    chk(true, false, "127.evil.com", false, true);
    // ── ?insecure_plaintext opt-out parse (boundary-sensitive) ──
    chk_opt("insecure_plaintext=1", true);
    chk_opt("insecure_plaintext=true", true);
    chk_opt("tls=insecure", false);       // TLS-insecure is SOMETHING ELSE (encrypted but unverified)
    chk_opt("foo=bar", false);
    chk_opt("", false);
    // #2: a substring inside the password/dbname does NOT SKIP REQUIRE_TLS (only a real key=truthy counts)
    chk_opt("password=my_insecure_plaintext=1x", false);
    chk_opt("dbname=insecure_plaintext=1db", false);
    chk_opt("insecure_plaintext=0", false);       // explicit falsey -> NOT an opt-out
    chk_opt("insecure_plaintext", false);         // no value -> NOT an opt-out (safe direction)
    // #2 end to end: with a substring inside the password, REMOTE plaintext is STILL REFUSED (no bypass)
    chk(true, false, "db.remote.com",
        db_insecure_plaintext_opt("password=z_insecure_plaintext=1"), true);

    // ── Log redaction (2026-09-30): the pool-open log used to print "mysql://root:PASSWORD" ──
    chk_redact("mysql://root:S3cretPw9@lkmy:3306/lookci", "mysql://root:***@lkmy:3306/lookci", "S3cretPw9");
    chk_redact("postgres://u:p4ss@h/db?sslmode=require",  "postgres://u:***@h/db?sslmode=require", "p4ss");
    chk_redact("mysqls://app:tok@db.example.com:3306/x?tls=verify",
               "mysqls://app:***@db.example.com:3306/x?tls=verify", "tok");
    chk_redact("redis://:pw9@h:6379",                     "redis://:***@h:6379", "pw9");
    // '@' and ':' in the password -- like the parser, the LAST '@' and the FIRST ':' -> no part leaks
    chk_redact("mysql://u:p@ss:w0rd@h/db",                "mysql://u:***@h/db", "w0rd");
    chk_redact("mysql://u:p@ss:w0rd@h/db",                "mysql://u:***@h/db", "p@ss");
    // no password -> unchanged (the user name / sqlite path does not change)
    chk_redact("mysql://root@h/db",                        "mysql://root@h/db", "");
    chk_redact("sqlite://:memory:",                        "sqlite://:memory:", "");
    chk_redact("sqlite:///var/app/data.db",                "sqlite:///var/app/data.db", "");

    printf("\n%d/%d PASS%s\n", ran - fails, ran, fails ? "  — FAIL VAR" : "");
    return fails ? 1 : 0;
}

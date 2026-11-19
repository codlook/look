#pragma once
// Pure DB DSN parsers — "open the seam" (same pattern as smtp_parse.h / imap_parse.h): separate the TLS decision
// logic from I/O → a single inline definition, table-testable (no TLS handshake / live DB).
#include <string>

namespace look {

// ── Boundary-aware DSN query-param lookup ────────────────────────────────────
// PROBLEM (adversarial parser testing, 2026-09-21): resolvers looked in the query with query.find("tls=insecure")
// i.e. a BARE SUBSTRING → silent mis-parse (a fuzzer would NOT catch it, no crash):
//   · ssl=false / ssl=0 / ssl=off  → the "ssl=" substring matched and FORCED TLS on (intent INVERTED)
//   · "insecure_plaintext=1" appearing inside a password/dbname SILENTLY SKIPPED REQUIRE_TLS
//   · ?dbname=x&opt=tls=insecure → in-value injection dropped verification
// FIX: match the key only at a param BOUNDARY (string start or after '&'), take the value up to '&'.
// dsn_lookup: true + value (val) if the key exists; false otherwise. A valueless key ("?x") → true, val="".
inline bool dsn_lookup(const std::string& q, const std::string& key, std::string& val) {
    val.clear();
    size_t n = q.size(), klen = key.size();
    size_t pos = 0;
    while (pos <= n) {
        if (pos + klen <= n && q.compare(pos, klen, key) == 0) {
            size_t after = pos + klen;
            if (after == n || q[after] == '=' || q[after] == '&') {   // param boundary
                if (after < n && q[after] == '=') {
                    size_t vend = q.find('&', after + 1);
                    val = q.substr(after + 1, vend == std::string::npos ? n - after - 1 : vend - after - 1);
                }
                return true;
            }
        }
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}
// Whether a DSN boolean value is "off" (explicit intent). Off: 0/false/off/no/disable/disabled.
// A valueless key ("?tls") or unknown value → NOT off (read presence as explicit on intent).
inline bool dsn_val_falsey(const std::string& v) {
    return v == "0" || v == "false" || v == "off" || v == "no" ||
           v == "disable" || v == "disabled";
}

// TLS decision from a PostgreSQL DSN query. scheme_secure = is it "postgresqls://".
//   → tls: will TLS be set up · verify: will cert+hostname be verified (SSL_VERIFY_PEER).
// In PG verify defaults to TRUE (unlike mysql/redis, it starts out safe).
//   insecure / sslmode=require        → tls, verify=false (encrypted, unverified — libpq 'require')
//   verify / sslmode=verify-full/-ca  → tls, verify=true  (CA+hostname; LOOK also tightens verify-ca to strict)
// CRITICAL (558d735): sslmode=verify-ca used to match NO branch → tls=scheme (FALSE in postgres://)
//   remained → SILENTLY PLAINTEXT. Now in the verify branch. This function's table test locks
//   that regression without a network.
inline void pg_resolve_tls(const std::string& query, bool scheme_secure,
                           bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // safe default
    std::string v;
    bool tls_kv = dsn_lookup(query, "tls", v);
    std::string sv; dsn_lookup(query, "ssl", sv);
    std::string mode; dsn_lookup(query, "sslmode", mode);
    bool insecure = (tls_kv && v == "insecure") || sv == "insecure" || mode == "require";
    bool wantverify = (tls_kv && (v == "verify" || v == "1" || v == "true")) ||
                      sv == "verify" || mode == "verify-full" || mode == "verify-ca"; // 558d735
    if (insecure) { tls = true; verify = false; }
    else if (wantverify) { tls = true; verify = true; }
}

// TLS decision from a MySQL/MariaDB DSN query. scheme_secure = is it "mysqls://"/"mariadbs://".
// SAFE DEFAULT (2026-08-11 flip): with TLS on, verify=true is the DEFAULT — aligned with http:: and
// PostgreSQL (two of the three drivers already verified; MySQL/Redis were the sole exception,
// a user who learned "LOOK verifies TLS" was SILENTLY wrong on MySQL). Connecting to a self-signed DB
// requires an EXPLICIT opt-out: ?tls=insecure.
//   tls=insecure / ssl=insecure                    → tls, verify=false (explicit opt-out, self-signed)
//   tls=verify / ssl=verify / ssl=verify_identity  → tls, verify=true  (explicit, redundant but harmless)
//   tls=1 / tls=true / any ssl=...                  → tls, verify=true  (safe default)
// BREAKING CHANGE: the old default was verify=false. Flipped while there were 0 users; it would have been
// impossible with the first real user. Measured: the known deployment (test.codlook.com) uses plaintext mysql://
// localhost → unaffected. The escape hatch ?tls=insecure (below) is ready.
inline void mysql_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // safe default (flip)
    std::string tv; bool has_tls = dsn_lookup(query, "tls", tv);
    std::string sv; bool has_ssl = dsn_lookup(query, "ssl", sv);
    if ((has_tls && tv == "insecure") || (has_ssl && sv == "insecure")) {
        tls = true; verify = false;   // explicit opt-out
    } else if ((has_tls && tv == "verify") ||
               (has_ssl && (sv == "verify" || sv == "verify_identity"))) {
        tls = true; verify = true;
    } else if ((has_tls && !dsn_val_falsey(tv)) ||
               (has_ssl && !dsn_val_falsey(sv))) {
        // tls=1/true OR any truthy ssl=... → enable TLS (the verify safe default is kept).
        // #1 fix: ssl=false/0/off → do NOT force TLS (explicit intent to disable is honored).
        tls = true;
    }
}

// TLS decision from a Redis DSN query. scheme_secure = is it "rediss://". Same SAFE DEFAULT
// philosophy as MySQL (verify=true default, ?tls=insecure explicit opt-out).
//   tls=insecure / ssl=insecure → tls, verify=false · tls=verify / ssl=verify → tls, verify=true
//   tls=1 / any ssl=...          → tls, verify=true (safe default)
inline void redis_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // safe default (flip)
    std::string tv; bool has_tls = dsn_lookup(query, "tls", tv);
    std::string sv; bool has_ssl = dsn_lookup(query, "ssl", sv);
    if ((has_tls && tv == "insecure") || (has_ssl && sv == "insecure")) {
        tls = true; verify = false;   // explicit opt-out
    } else if ((has_tls && tv == "verify") || (has_ssl && sv == "verify")) {
        tls = true; verify = true;
    } else if ((has_tls && !dsn_val_falsey(tv)) ||
               (has_ssl && !dsn_val_falsey(sv))) {
        tls = true;   // the verify safe default (true) is kept (#1 fix: falsey does NOT force)
    }
}

// ── Pool-open DB-TLS warning ───────────────────────────────────────────────
// PROBLEM (2026-09-30): the warning in web_stdlib searched the raw DSN for a BARE SUBSTRING (the same class
// 739d581 closed, this time in the WARNING path) → "tls=" in a password silenced the plaintext warning,
// ?ssl=false counted as "TLS present", ?x=tls=insecure raised a false warning
// on plaintext. FIX: extract the query part from the same place as the REAL parser (never userinfo) and
// ask the real resolvers for the decision → the warning matches the established connection exactly.
// Query extraction: mysql/pg = parse_dsn_part (after the last '@', db after the first '/', first '?' within it);
// redis = resp_client parse_url (first '?' after the scheme).
// Splits the post-scheme part of a Redis URL: userinfo ("user:pass", empty if no '@'),
// hostpart ("host:port/db") and query. LAST '@' first (same rule as the mysql/pg parser),
// THEN '?'. OLD BUG: '?' was searched first → for a password containing '?' like
// "redis://:pa?ss@host:6379" the query came out "ss@host:6379" and host ":pa": an attempt to connect
// to the wrong server name + half the password parsed as a TLS option.
inline void redis_split_url(const std::string& after_scheme, std::string& userinfo,
                            std::string& hostpart, std::string& query) {
    userinfo.clear(); query.clear();
    hostpart = after_scheme;
    const size_t at = hostpart.rfind('@');
    if (at != std::string::npos) { userinfo = hostpart.substr(0, at); hostpart = hostpart.substr(at + 1); }
    const size_t q = hostpart.find('?');
    if (q != std::string::npos) { query = hostpart.substr(q + 1); hostpart = hostpart.substr(0, q); }
}
inline std::string db_dsn_query(const std::string& dsn) {
    const size_t se = dsn.find("://");
    if (se == std::string::npos) return "";
    const std::string sch = dsn.substr(0, se);
    std::string rest = dsn.substr(se + 3);
    if (sch == "redis" || sch == "rediss") {
        std::string ui, hp, query;
        redis_split_url(rest, ui, hp, query);
        return query;
    }
    size_t at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    // SAME split as parse_dsn_part: the authority ends at the first '/' or '?' (Go net/url).
    // Previously only '/' was searched → for pathless "h:3306?tls=1" the connection set up TLS while this warning
    // saw an empty query and said "plaintext" (warning and real connection diverged).
    size_t q = rest.find('?');
    return q == std::string::npos ? "" : rest.substr(q + 1);
}
// Empty return → no warning. Texts are user-visible (English).
inline std::string db_tls_warning(const std::string& dsn) {
    const std::string sch = dsn.substr(0, dsn.find(':'));
    const std::string query = db_dsn_query(dsn);
    bool tls = false, verify = true;
    if (sch == "postgres" || sch == "postgresql" || sch == "postgresqls") {
        pg_resolve_tls(query, sch == "postgresqls", tls, verify);
        if (!tls)
            return "PostgreSQL connection is UNENCRYPTED (plaintext) — for encrypted+verified use: postgresqls://... or add ?tls=verify to the DSN.";
        if (!verify)
            return "PostgreSQL TLS certificate is NOT VERIFIED (MITM risk) — use postgresqls:// or ?tls=verify instead of ?tls=insecure.";
        return "";
    }
    const char* name;
    if (sch == "mysql" || sch == "mariadb" || sch == "mysqls" || sch == "mariadbs") {
        mysql_resolve_tls(query, sch == "mysqls" || sch == "mariadbs", tls, verify);
        name = "MySQL";
    } else if (sch == "redis" || sch == "rediss") {
        redis_resolve_tls(query, sch == "rediss", tls, verify);
        name = "Redis";
    } else {
        return "";   // sqlite etc. — no network
    }
    if (!tls)
        return std::string(name) + " connection is UNENCRYPTED (plaintext) — for encrypted+verified use: " +
               (name[0] == 'R' ? "rediss://..." : "mysqls://...");
    if (!verify)
        return std::string(name) +
               " TLS certificate is NOT VERIFIED with ?tls=insecure (MITM risk) — remove insecure if possible (verify is now the default).";
    return "";
}

// ── Masking the DSN for log/error ──────────────────────────────────────────
// SECURITY (2026-09-30, measured on live mysql:8): the pool-open INFO log trimmed the DSN with
// dsn.substr(0, dsn.find('@')) — the intent was to hide credentials but it took the part BEFORE @
// → the log got exactly "mysql://root:PASSWORD" (the secret was printed, the non-secret host was hidden). Masking uses
// EXACTLY the same split as the parser (web_stdlib parse_dsn_part): userinfo = after "://" up to the LAST '@',
// password = after the FIRST ':' in userinfo → even if the password contains '@' or ':', no part of it
// leaks. If there is no password (user@host, sqlite://...) the DSN is returned unchanged.
inline std::string db_dsn_redact(const std::string& dsn) {
    const size_t se = dsn.find("://");
    if (se == std::string::npos) return dsn;
    const size_t rs = se + 3;
    const size_t at = dsn.rfind('@');
    if (at == std::string::npos || at < rs) return dsn;          // no credentials
    const size_t colon = dsn.find(':', rs);
    if (colon == std::string::npos || colon > at) return dsn;    // user present, no password
    return dsn.substr(0, colon + 1) + "***" + dsn.substr(at);
}

// ── Require-TLS policy (LOOK_DB_REQUIRE_TLS) ──────────────────────────────────
// Opt-in operator policy: REJECT a PLAINTEXT connection to a REMOTE host. Loopback /
// Unix-socket plaintext is ALWAYS allowed (traffic does not leave the machine — the common, safe
// app+DB-on-same-machine setup) → the policy never locks out that setup. Escape for deliberately-trusted
// remote plaintext: ?insecure_plaintext=1 in the DSN (named like ?tls=insecure
// → "I know this is insecure" is VISIBLE in log/config; not a neutral "allow"). Loopback detection is
// STRING-BASED on the literal host (deterministic, NO DNS-resolution/spoofing surface): a hostname that resolves to
// 127.0.0.1 counts as REMOTE → write the IP literal or use ?insecure_plaintext=1. (Documented in SECURITY.md.)
inline bool db_host_is_local(const std::string& host) {
    if (host.empty() || host[0] == '/' ||     // unix socket / unspecified / socket path
        host == "localhost" ||
        host == "::1" || host == "[::1]") return true;
    // 127.0.0.0/8 — but ONLY an IPv4 LITERAL (digits+dots only). #3 fix: a hostname resolving REMOTE
    // like "127.evil.com" used to count as local via the "127." prefix and skipped REQUIRE_TLS.
    // A hostname (contains letters) now counts as remote.
    return host.rfind("127.", 0) == 0 &&
           host.find_first_not_of("0123456789.") == std::string::npos;
}
inline bool db_insecure_plaintext_opt(const std::string& query) {
    // #2 fix: boundary-aware — an "insecure_plaintext=1" substring inside a password/dbname
    // no longer SILENTLY SKIPS REQUIRE_TLS. Only a real key=truthy-value counts.
    std::string v;
    return dsn_lookup(query, "insecure_plaintext", v) && !dsn_val_falsey(v) && !v.empty();
}
// true → the connection MUST BE REJECTED. require_tls: LOOK_DB_REQUIRE_TLS on ·
// encrypted: TLS will be set up · insecure_plaintext: ?insecure_plaintext=1 opt-out.
inline bool db_refuse_plaintext(bool require_tls, bool encrypted,
                                const std::string& host, bool insecure_plaintext) {
    if (!require_tls || encrypted || insecure_plaintext) return false;
    return !db_host_is_local(host);
}

} // namespace look

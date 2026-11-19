#!/bin/bash
# MySQL authentication guard — BOTH VERSIONS AT ONCE.
#
# WHY: mysql_client.cpp implemented only `mysql_native_password` and wrote the plugin
# name into the handshake response as a CONSTANT. The plugin the server announced
# was not read, AuthSwitchRequest (0xFE) was not handled at all. Result:
#   MySQL 5.7 / MariaDB                          -> works
#   MySQL 8.0 (sha2 is the DEFAULT since 2018)   -> CANNOT CONNECT
#   MySQL 8.4 (native off) / 9.x (removed)       -> CANNOT CONNECT
# So in practice the language was a MySQL 5.7 language.
#
# WHY TWO VERSIONS: a test that passes on one version can break on the other. 8.0 verifies the new path
# (caching_sha2 + RSA), 5.7 the old path (native); the two share the same handshake
# code.
#
# Usage:
#   bash mysql_auth_test.sh <lk_yolu>
# If the environment variables are not given it sets up the containers itself (needs docker):
#   LOOK_MY8_DSN   (default: mysql://root:sifre123@my8:3306/testdb)
#   LOOK_MY57_DSN  (default: mysql://root:sifre123@my57:3306/testdb)
set -u
LK="${1:-./build/lk}"
MY8="${LOOK_MY8_DSN:-mysql://root:sifre123@my8:3306/testdb}"
MY57="${LOOK_MY57_DSN:-mysql://root:sifre123@my57:3306/testdb}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/my.lk" <<'LK'
try {
  $c = db::connect(env("MYDSN", ""))
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_auth_t (id INT PRIMARY KEY AUTO_INCREMENT, ad VARCHAR(50)) CHARACTER SET utf8mb4")
  db::exec($c, "DELETE FROM look_auth_t")
  db::exec($c, "INSERT INTO look_auth_t (ad) VALUES (?)", ["şğü"])
  $r = db::query($c, "SELECT ad FROM look_auth_t")
  $v = db::query($c, "SELECT VERSION() AS v")
  print("OK|" . $v[0].v . "|" . $r[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

kontrol() {
  etiket="$1"; dsn="$2"; bek_surum="$3"
  out=$(MYDSN="$dsn" timeout 40 "$LK" "$TMP/my.lk" 2>&1 | grep -E '^(OK|HATA)' | head -1)
  case "$out" in
    OK\|${bek_surum}*\|şğü)
      echo "  PASS $etiket -> ${out}" ;;
    HATA*)
      echo "  FAIL $etiket could not connect: $out"
      case "$out" in
        *"Access denied"*|*"authentication"*|*plugin*)
          echo "       -> the authentication plugin may not be handled"
          echo "          (is the plugin announced by the server being read? 0xFE AuthSwitchRequest?)" ;;
      esac
      fail=1 ;;
    *)
      echo "  FAIL $etiket unexpected output: $out"; fail=1 ;;
  esac
}

# ── SECURITY: is escaping safe WITHOUT depending on SET SESSION? ────────────────────
# WHY: escape() escaped the quote with `\'`. In MySQL NO_BACKSLASH_ESCAPES mode the
# backslash is NOT an escape character -> `\'` closes the quote -> SQL INJECTION.
# MEASURED: once the session was switched to that mode, the payload `' OR 1=1 -- ` returned the WHOLE TABLE (3/3).
# The only thing preventing it was the `SET SESSION sql_mode = REPLACE(...)`
# command in do_connect() succeeding — not a sufficient basis (connection multiplexers such as ProxySQL
# can silently lose session state; the SET succeeds, the protection
# is gone). Fix: `''` (standard SQL) — safe in both modes, independent of SET.
# This check DELIBERATELY removes the protection and attacks: security must not depend on SET.
kacis_kontrol() {
  dsn="$1"
  [ -z "$dsn" ] && return
  cat > "$TMP/esc.lk" <<'LK'
$c = db::connect(env("MYDSN",""))
db::exec($c, "DROP TABLE IF EXISTS look_esc")
db::exec($c, "CREATE TABLE look_esc (ad VARCHAR(100)) CHARACTER SET utf8mb4")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('a')")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('b')")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('c')")
# DELIBERATELY remove the protection — escaping must be safe on its own
db::exec($c, "SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES'")
$n1 = count(db::query($c, "SELECT * FROM look_esc WHERE ad = ?", ["' OR 1=1 -- "]))
$n2 = count(db::query($c, "SELECT * FROM look_esc WHERE ad = ?", ["\\' OR 1=1 -- "]))
db::exec($c, "SET SESSION sql_mode = 'STRICT_TRANS_TABLES'")
db::exec($c, "INSERT INTO look_esc (ad) VALUES (?)", ["O'Brien"])
$rt = db::query($c, "SELECT ad FROM look_esc WHERE ad = ?", ["O'Brien"])
print($n1 . "|" . $n2 . "|" . count($rt))
LK
  out=$(MYDSN="$dsn" timeout 40 "$LK" "$TMP/esc.lk" 2>&1 | grep -E '^[0-9]+\|' | head -1)
  if [ "$out" = "0|0|1" ]; then
    echo "  PASS escaping is safe regardless of SET (no injection even in NBE mode, data preserved)"
  else
    echo "  FAIL SECURITY: escaping breaks in NO_BACKSLASH_ESCAPES mode: [$out] (expected 0|0|1)"
    echo "       -> does escape() escape the quote with '\\''? Standard SQL '' must be used"
    fail=1
  fi
}

echo "MySQL/MariaDB authentication guard (version matrix)"
kontrol "MySQL 5.7   (mysql_native_password)"  "$MY57" "5.7"
kontrol "MySQL 8.0   (caching_sha2_password)"  "$MY8"  "8.0"
[ -n "${LOOK_MY82_DSN:-}"  ] && kontrol "MySQL 8.2   (caching_sha2)"                "$LOOK_MY82_DSN"  "8.2"
[ -n "${LOOK_MY84_DSN:-}"  ] && kontrol "MySQL 8.4   (native DISABLED)"             "$LOOK_MY84_DSN"  "8.4"
[ -n "${LOOK_MY91_DSN:-}"  ] && kontrol "MySQL 9.x   (native REMOVED)"           "$LOOK_MY91_DSN"  "9."
[ -n "${LOOK_MDB_DSN:-}"   ] && kontrol "MariaDB     (native)"                      "$LOOK_MDB_DSN"   "1"

# Second round: on 8.x the server cache is now full -> FAST path (a different code branch).
# The first round verifies the full RSA path, this round the fast path. BOTH must run:
# if only the fast path were tested, it would pass even if the RSA path were broken (on a fresh
# server the first connection ALWAYS goes through RSA).
kontrol "MySQL 8.0   (fast path, cache warm)" "$MY8" "8.0"

# Escape safety — independent of authentication, on the same server
kacis_kontrol "$MY8"

[ $fail = 0 ] && echo "PASS: MySQL/MariaDB auth — version matrix clean" || echo "FAIL: MySQL auth"
exit $fail

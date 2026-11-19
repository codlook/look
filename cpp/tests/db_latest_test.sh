#!/bin/bash
# DB VERSION-AWARENESS guard — MySQL / MariaDB / PostgreSQL / SQLite.
#
# GOAL: to catch the "a new DB version came out and broke LOOK" case the day the version
# ships, before a USER complains. MySQL 8 moving to caching_sha2_password was exactly a
# surprise of this class (bug 39) — the language could silently no longer connect.
#
# How it works: connects to each DB's `latest`-tagged container, does write/read +
# a version query. If run weekly in CI, it turns FAIL red when upstream brings a break.
# `latest` is DIFFERENT from today's pinned matrix (mysql_auth_test.sh):
# that one locks the tested versions, this one tracks the future.
#
# Usage:
#   bash db_latest_test.sh <lk_path>
# DSNs come from the environment (a DB that is not given is skipped with a visible message — not silently):
#   LOOK_MYSQL_LATEST_DSN   mysql://root:pw@host:3306/db
#   LOOK_MARIADB_LATEST_DSN mysql://root:pw@host:3306/db
#   LOOK_PG_LATEST_DSN      postgres://user:pw@host:5432/db
#   (SQLite is embedded — needs no DSN, always runs)
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# MySQL/MariaDB (same wire) — utf8mb4, Turkish text must survive the round trip
cat > "$TMP/my.lk" <<'LK'
try {
  $c = db::connect(env("D",""))
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_lt (id INT PRIMARY KEY AUTO_INCREMENT, ad VARCHAR(50)) CHARACTER SET utf8mb4")
  db::exec($c, "DELETE FROM look_lt")
  db::exec($c, "INSERT INTO look_lt (ad) VALUES (?)", ["şğü"])
  print("OK|" . db::query($c,"SELECT VERSION() AS v")[0].v . "|" . db::query($c,"SELECT ad FROM look_lt")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

# PostgreSQL — SERIAL/TEXT + a table WITHOUT a sequence in a transaction (39... lastval data loss)
# WHY: in PG, SELECT lastval() is called automatically after INSERT; if the table does not use SERIAL
# lastval errors and aborts the OPEN TRANSACTION -> COMMIT
# silently becomes ROLLBACK -> data is lost. This check does INSERT + COMMIT inside a transaction into a table WITHOUT a sequence
# and verifies that the data REMAINS.
cat > "$TMP/pg.lk" <<'LK'
use string
try {
  $c = db::connect(env("D",""))
  # table WITHOUT a sequence + transaction — lastval data-loss trap.
  # NOTE: this test must come BEFORE the SERIAL insert. Otherwise the SERIAL insert
  # DEFINES lastval in the session and lastval no longer errors on an insert without a sequence
  # -> the bug is not triggered (the guard gives a misleading PASS — this is exactly what happened).
  db::exec($c, "DROP TABLE IF EXISTS look_notx")
  db::exec($c, "CREATE TABLE look_notx (n INT)")
  db::begin($c)
  db::exec($c, "INSERT INTO look_notx (n) VALUES (1)")
  db::commit($c)
  $kaldi = count(db::query($c, "SELECT n FROM look_notx"))
  if ($kaldi != 1) { print("HATA|in-transaction INSERT was lost on a table without a sequence (lastval abort): " . $kaldi . " rows"); return }
  # SERIAL test (last_id + version)
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_lt (id SERIAL PRIMARY KEY, ad TEXT)")
  db::exec($c, "DELETE FROM look_lt")
  db::exec($c, "INSERT INTO look_lt (ad) VALUES (?)", ["şğü"])
  print("OK|" . string::substr(db::query($c,"SELECT version() AS v")[0].v, 0, 20) . "|" . db::query($c,"SELECT ad FROM look_lt")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

# SQLite — embedded, no DSN
cat > "$TMP/sq.lk" <<'LK'
use string
try {
  $c = db::connect("sqlite://:memory:")
  db::exec($c, "CREATE TABLE t (id INTEGER PRIMARY KEY, ad TEXT)")
  db::exec($c, "INSERT INTO t (ad) VALUES (?)", ["şğü"])
  print("OK|" . db::query($c,"SELECT sqlite_version() AS v")[0].v . "|" . db::query($c,"SELECT ad FROM t")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

kontrol() {
  etiket="$1"; script="$2"; dsn="${3:-}"
  if [ "$script" != "$TMP/sq.lk" ] && [ -z "$dsn" ]; then
    echo "  SKIPPED $etiket (no DSN given)"; return
  fi
  out=$(D="$dsn" timeout 45 "$LK" "$script" 2>&1 | grep -E '^(OK|HATA)' | head -1)
  case "$out" in
    OK\|*\|şğü) echo "  PASS $etiket -> ${out#OK|}" ;;
    *)          echo "  FAIL $etiket -> $out"; fail=1 ;;
  esac
}

echo "DB latest version-awareness guard"
kontrol "MySQL   latest" "$TMP/my.lk" "${LOOK_MYSQL_LATEST_DSN:-}"
kontrol "MariaDB latest" "$TMP/my.lk" "${LOOK_MARIADB_LATEST_DSN:-}"
kontrol "Postgres latest" "$TMP/pg.lk" "${LOOK_PG_LATEST_DSN:-}"
kontrol "SQLite  (built-in)" "$TMP/sq.lk"

[ $fail = 0 ] && echo "PASS: DB latest — every reachable version is clean" || echo "FAIL: DB latest"
exit $fail

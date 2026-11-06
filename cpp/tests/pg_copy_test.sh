#!/usr/bin/env bash
# PostgreSQL COPY ... FROM STDIN / TO STDOUT through db:: fails at once with the reason, and the
# connection stays usable.
#
# db:: does not carry a COPY data stream. For FROM STDIN the server waited for data and the client
# for an answer, until the read timeout (30 s) ended it with an unrelated error. TO STDOUT dropped
# the rows and returned an empty result. Now the client answers the server's COPY request with
# CopyFail (FROM STDIN) or reads the stream to its end (TO STDOUT) and raises a clear error.
# Usage: pg_copy_test.sh <lk> <pg-dsn>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
export DSN="${2:?usage}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat > "$T/c.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
db::exec($c, "DROP TABLE IF EXISTS lk_copy", [])
db::exec($c, "CREATE TABLE lk_copy (id INT)", [])
db::exec($c, "INSERT INTO lk_copy VALUES (1), (2)", [])
try { db::exec($c, "COPY lk_copy FROM STDIN", []); print("in: ran") } catch ($e) { print("in: " . $e) }
print("after-in: " . db::query($c, "SELECT COUNT(*) AS n FROM lk_copy", [])[0]["n"])
try { $r = db::query($c, "COPY lk_copy TO STDOUT", []); print("out: rows " . count($r)) } catch ($e) { print("out: " . $e) }
print("after-out: " . db::query($c, "SELECT COUNT(*) AS n FROM lk_copy", [])[0]["n"])
db::begin($c)
try { db::exec($c, "COPY lk_copy FROM STDIN", []) } catch ($e) { print("tx-in: error") }
db::rollback($c)
print("after-tx: " . db::query($c, "SELECT 7 AS n", [])[0]["n"])
LK
s=$(date +%s); out="$(cd "$T" && timeout 100 "$LK" c.lk 2>&1 | grep -v '^\[20')"; dt=$(( $(date +%s) - s ))
echo "$out" | sed 's/^/  ..   /'
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }
echo "$out" | grep -q "^in: .*COPY FROM STDIN is not supported" && ok "COPY FROM STDIN is an error naming the reason" || bad "COPY FROM STDIN"
echo "$out" | grep -q "^out: .*COPY TO STDOUT is not supported" && ok "COPY TO STDOUT is an error, not an empty result" || bad "COPY TO STDOUT"
echo "$out" | grep -q "^after-in: 2" && echo "$out" | grep -q "^after-out: 2" && echo "$out" | grep -q "^after-tx: 7" \
    && ok "the connection is usable after each" || bad "the connection after COPY"
[ $dt -lt 10 ] && ok "all of it took ${dt} s (no wait for a timeout)" || bad "took ${dt} s"
[ $fail = 0 ] && echo "PASS: COPY through db:: fails at once and leaves the connection usable" \
             || { echo "FAIL: PostgreSQL COPY through db::"; exit 1; }

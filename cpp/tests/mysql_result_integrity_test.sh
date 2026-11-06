#!/usr/bin/env bash
# MySQL: every query gets its own complete answer, or an error — never a partial or a stale one.
#
#   - An error the server sends in the middle of the rows (a query stopped after it had begun
#     answering) ended the row loop silently: the first rows came back as if they were the whole
#     result. Now it is the error.
#   - CALL of a procedure answers with its result set and then one more OK packet (the server marks
#     "more results"). That packet was left unread; the next query saw leftover bytes and dropped
#     the connection — a new connection for every CALL, and inside a transaction the transaction
#     was lost ("the connection was closed inside a transaction"). All results are now read; the
#     first set is returned.
#   - The client no longer asks for multi-statement text queries or LOCAL INFILE, which nothing in
#     LOOK uses (user SQL always goes as a prepared statement).
# Needs a live MySQL server and a user allowed to CREATE PROCEDURE.
# Usage: mysql_result_integrity_test.sh <lk> <lk-fcgi> <mysql-dsn>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
export DSN="${3:?usage}" LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
PORT="${PORT:-7761}"
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }

cat > "$T/setup.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
db::exec($c, "DROP TABLE IF EXISTS lk_integrity", [])
db::exec($c, "CREATE TABLE lk_integrity (id INT PRIMARY KEY)", [])
db::exec($c, "INSERT INTO lk_integrity WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 1000) SELECT n FROM s", [])
db::exec($c, "INSERT INTO lk_integrity SELECT id + 1000 FROM lk_integrity", [])
$p = db::query($c, "SELECT COUNT(*) AS n FROM information_schema.routines WHERE routine_schema = DATABASE() AND routine_name = 'lk_two'", [])
print($p[0]["n"] == 1 ? "ready" : "no-proc")
LK
# CREATE PROCEDURE hazırlanmış ifade protokolünde yok → mysql istemcisiyle (DSN'den)
if command -v mysql >/dev/null 2>&1; then
    r="${DSN#mysql://}"; r="${r%%\?*}"; cred="${r%%@*}"; hp="${r#*@}"; hp="${hp%%/*}"; db="${r##*/}"
    MYSQL_PWD="${cred#*:}" mysql -h "${hp%%:*}" -P "${hp##*:}" -u "${cred%%:*}" "$db" \
        -e "DROP PROCEDURE IF EXISTS lk_two; CREATE PROCEDURE lk_two() SELECT 7 AS x;" 2>&1 | grep -v Warning
fi
s="$(cd "$T" && timeout 120 "$LK" setup.lk 2>&1 | grep -v '^\[20' | tail -1)"
[ "$s" = ready ] || { echo "FAIL: setup ($s) — needs the mysql client to create the procedure lk_two"; exit 1; }

cat > "$T/app.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
# tek bağlantı: CALL'dan kalan paket aynı bağlantıdaki sonraki sorguya gider
route("GET", "/call", function() use ($c) {
    $a = db::query($c, "CALL lk_two()", [])
    $b = db::query($c, "SELECT 2 AS y", [])
    return response::text(json::encode([$a, $b]))
})
# satır 1500'de alt sorgu iki satır döndürür → ER_SUBQUERY_NO_1_ROW, ilk satırlar gönderildikten SONRA
route("GET", "/mid", function() use ($c) {
    try {
        $r = db::query($c, "SELECT id FROM lk_integrity WHERE IF(id = 1500, (SELECT 1 UNION SELECT 2), 1)", [])
        return response::text("rows " . count($r))
    } catch ($e) {
        return response::text("error " . $e)
    }
})
# İşlem içinde: eskiden kalan paket bağlantıyı "bozuk" sayıp kapattırıyordu → işlem geri alınıyordu
route("GET", "/txcall", function() use ($c) {
    try {
        db::begin($c)
        db::exec($c, "DELETE FROM lk_integrity WHERE id = 5000", [])
        db::exec($c, "INSERT INTO lk_integrity VALUES (5000)", [])
        db::query($c, "CALL lk_two()", [])
        db::exec($c, "UPDATE lk_integrity SET id = 5001 WHERE id = 5000", [])
        db::commit($c)
        return response::text(json::encode(db::query($c, "SELECT id FROM lk_integrity WHERE id >= 5000", [])))
    } catch ($e) {
        try { db::rollback($c) } catch ($e2) {}
        return response::text("error " . $e)
    }
})
route("GET", "/after", fn() => response::text(json::encode(db::query($c, "SELECT 3 AS z", []))))
route("GET", "/multi", function() use ($c) {
    try { db::query($c, "SELECT 1; SELECT 2", []); return response::text("ran") }
    catch ($e) { return response::text("refused") }
})
LK
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 100); do curl -s -o /dev/null -m 1 "localhost:$PORT/after" && break; sleep 0.1; done
U="http://localhost:$PORT"

r="$(curl -s "$U/call")"
echo "$r" | grep -q '"x":7' && echo "$r" | grep -q '"y":2' \
    && ok "CALL returns its rows and the next query gets its own answer" || bad "CALL left its trailing packet: $r"
r="$(curl -s "$U/call")"
echo "$r" | grep -q '"y":2' && ok "again on the same connection" || bad "second CALL round: $r"
r="$(curl -s "$U/txcall")"
[ "$r" = '[{"id":5001}]' ] && ok "CALL inside a transaction keeps the transaction" || bad "CALL inside a transaction: $r"

r="$(curl -s "$U/mid")"
case "$r" in
  error*) ok "an error in the middle of the rows is an error ($(echo "$r" | cut -c1-60))" ;;
  *)      bad "an error in the middle of the rows came back as a partial result: $r" ;;
esac
r="$(curl -s "$U/after")"
echo "$r" | grep -q '"z":3' && ok "the connection is usable after that error" || bad "after the error: $r"
[ "$(curl -s "$U/multi")" = refused ] && ok "a multi-statement query is refused" || bad "a multi-statement query ran"
grep -q "route disable" "$T/log.txt" && bad "a route fell back to the interpreter" || true

[ $fail = 0 ] && echo "PASS: MySQL answers are complete and never carried to the next query" \
             || { echo "FAIL: MySQL result integrity"; exit 1; }

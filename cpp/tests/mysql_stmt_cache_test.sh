#!/usr/bin/env bash
# MySQL: a statement is prepared once per connection, and the cache never returns a wrong result.
#
# db::query / db::exec used to send PREPARE, EXECUTE and CLOSE for every call — two round trips —
# and read every packet with two recv calls (header, then body). Now each connection keeps the
# statements it prepared (one round trip per query) and reads through a buffer. A cache that
# returns a wrong result is worse than none, so this test covers each way it could:
#   - a table that changes shape (ALTER, DROP + CREATE) is read with its new columns;
#   - two connections to two databases (one per shop) never see each other's rows;
#   - a connection the server dropped comes back without its old statement ids;
#   - the server's global statement limit (max_prepared_stmt_count) is never exhausted by the
#     cache, and when other clients exhaust it the query still runs;
#   - a rolled-back transaction leaves nothing behind;
#   - 64 concurrent web requests each get their own row.
# Needs a live MySQL server and a user allowed to CREATE DATABASE, KILL and SET GLOBAL (root in CI).
# Usage: mysql_stmt_cache_test.sh <lk> <lk-fcgi> <mysql-dsn ending in /database>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
DSN="${3:?usage}"; BASE="${DSN%/*}"
export DSN BASE LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; printf "use db\n\$c = db::connect(env(\"DSN\"))\ndb::exec(\$c, \"SET GLOBAL max_prepared_stmt_count = 16382\", [])\n" > "$T/restore.lk"; "$LK" "$T/restore.lk" >/dev/null 2>&1; rm -rf "$T"' EXIT
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }
run() { (cd "$T" && timeout 120 "$LK" "$1" 2>&1 | grep -v '^\[20'); }

cat > "$T/setup.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
db::exec($c, "DROP DATABASE IF EXISTS lk_shop_a", [])
db::exec($c, "DROP DATABASE IF EXISTS lk_shop_b", [])
db::exec($c, "CREATE DATABASE lk_shop_a", [])
db::exec($c, "CREATE DATABASE lk_shop_b", [])
db::exec($c, "CREATE TABLE lk_shop_a.items (id INT PRIMARY KEY, v VARCHAR(20))", [])
db::exec($c, "CREATE TABLE lk_shop_b.items (id INT PRIMARY KEY, v VARCHAR(20))", [])
for ($i = 1; $i <= 50; $i = $i + 1) {
    db::exec($c, "INSERT INTO lk_shop_a.items VALUES (?, ?)", [$i, "a" . $i])
    db::exec($c, "INSERT INTO lk_shop_b.items VALUES (?, ?)", [$i, "b" . $i])
}
db::exec($c, "DROP TABLE IF EXISTS lk_cache_t", [])
db::exec($c, "CREATE TABLE lk_cache_t (id INT PRIMARY KEY, name VARCHAR(20))", [])
db::exec($c, "INSERT INTO lk_cache_t VALUES (1, 'one'), (2, 'two')", [])
print("setup ok")
LK
out=$(run setup.lk); [ "$out" = "setup ok" ] || { echo "FAIL: setup: $out"; exit 1; }

echo "-- repeated statements --"
cat > "$T/repeat.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
$bad = 0
for ($i = 0; $i < 500; $i = $i + 1) {
    $id = ($i % 50) + 1
    $r = db::query($c, "SELECT v FROM lk_shop_a.items WHERE id = ?", [$id])
    if (count($r) != 1 || $r[0]["v"] != "a" . $id) { $bad = $bad + 1 }
}
print("bad=" . $bad)
LK
out=$(run repeat.lk); [ "$out" = "bad=0" ] && ok "500 runs of one statement with changing parameters: every row right" || bad "repeated statement: $out"

echo "-- a table that changes shape --"
cat > "$T/shape.lk" <<'LK'
use db
use json
$c = db::connect(env("DSN"))
$q = "SELECT * FROM lk_cache_t WHERE id = ?"
print(json::encode(db::query($c, $q, [1])))
db::exec($c, "ALTER TABLE lk_cache_t ADD COLUMN price INT DEFAULT 7", [])
print(json::encode(db::query($c, $q, [1])))
db::exec($c, "ALTER TABLE lk_cache_t DROP COLUMN name", [])
print(json::encode(db::query($c, $q, [1])))
db::exec($c, "DROP TABLE lk_cache_t", [])
db::exec($c, "CREATE TABLE lk_cache_t (id INT PRIMARY KEY, label TEXT, n DOUBLE)", [])
db::exec($c, "INSERT INTO lk_cache_t VALUES (1, 'new', 2.5)", [])
print(json::encode(db::query($c, $q, [1])))
LK
want='[{"id":1,"name":"one"}]
[{"id":1,"name":"one","price":7}]
[{"id":1,"price":7}]
[{"id":1,"label":"new","n":2.5}]'
out=$(run shape.lk); [ "$out" = "$want" ] && ok "ALTER ADD/DROP COLUMN and DROP + CREATE: the same statement returns the new columns" \
    || { bad "table shape change"; echo "$out" | sed 's/^/      got: /'; }

echo "-- one connection per shop --"
cat > "$T/shops.lk" <<'LK'
use db
$a = db::connect(env("BASE") . "/lk_shop_a")
$b = db::connect(env("BASE") . "/lk_shop_b")
$bad = 0
for ($i = 1; $i <= 50; $i = $i + 1) {
    $ra = db::query($a, "SELECT v FROM items WHERE id = ?", [$i])
    $rb = db::query($b, "SELECT v FROM items WHERE id = ?", [$i])
    if ($ra[0]["v"] != "a" . $i || $rb[0]["v"] != "b" . $i) { $bad = $bad + 1 }
}
print("bad=" . $bad)
LK
out=$(run shops.lk); [ "$out" = "bad=0" ] && ok "the same SQL text on two shops' databases: each sees only its own rows" || bad "shops crossed: $out"

echo "-- a connection the server dropped --"
cat > "$T/killed.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
$k = db::connect(env("BASE") . "/lk_shop_b")   // başka DSN = başka havuz (aynı DSN aynı bağlantıyı paylaşırdı)
$q = "SELECT v FROM lk_shop_a.items WHERE id = ?"
$r1 = db::query($c, $q, [3])
$me = db::query($c, "SELECT CONNECTION_ID() AS id", [])[0]["id"]
db::exec($k, "KILL " . $me, [])
// İfade kimlikleri her bağlantıda 1'den başlar: yeni bağlantıda önce BAŞKA bir ifade hazırlanır
// (eski kimliği o alır). Önbellek eski kimliği tutsaydı aşağıdaki $q bu ifadeyi çalıştırırdı —
// hata yok, yanlış veri.
$other = db::query($c, "SELECT 'other' AS v FROM lk_shop_b.items WHERE id = ?", [1])
$r2 = db::query($c, $q, [4])
$me2 = db::query($c, "SELECT CONNECTION_ID() AS id", [])[0]["id"]
print($r1[0]["v"] . "|" . $other[0]["v"] . "|" . $r2[0]["v"] . "|" . ($me2 != $me ? "new-connection" : "same-connection"))
LK
out=$(run killed.lk); [ "$out" = "a3|other|a4|new-connection" ] && ok "KILLed connection: replaced; statement ids restart there, and the cached statement still returns its own rows" || bad "after KILL: $out"

echo "-- the server's statement limit --"
cat > "$T/limit.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
$k = db::connect(env("DSN"))
$before = db::query($k, "SHOW GLOBAL STATUS LIKE 'Prepared_stmt_count'", [])[0]["Value"]
$bad = 0
for ($i = 1; $i <= 300; $i = $i + 1) {
    $r = db::query($c, "SELECT " . $i . " AS n, v FROM lk_shop_a.items WHERE id = ?", [1])
    if ($r[0]["n"] != $i) { $bad = $bad + 1 }
}
$after = db::query($k, "SHOW GLOBAL STATUS LIKE 'Prepared_stmt_count'", [])[0]["Value"]
print("bad=" . $bad . " grew=" . ($after - $before))
LK
out=$(run limit.lk); bad_n=$(echo "$out" | sed -n 's/^bad=\([0-9]*\).*/\1/p'); grew=$(echo "$out" | sed -n 's/.*grew=\(-*[0-9]*\).*/\1/p')
[ "$bad_n" = 0 ] && [ -n "$grew" ] && [ "$grew" -le 66 ] && ok "300 different statements on one connection: the server holds at most 64 of them (grew by $grew)" \
    || bad "statement limit: $out"
cat > "$T/full.lk" <<'LK'
use db
$admin = db::connect(env("DSN"))
db::exec($admin, "SET GLOBAL max_prepared_stmt_count = 3", [])
$c = db::connect(env("DSN"))
$bad = 0
$err = ""
for ($i = 1; $i <= 20; $i = $i + 1) {
    try {
        $r = db::query($c, "SELECT " . $i . " AS n FROM lk_shop_a.items WHERE id = ?", [1])
        if ($r[0]["n"] != $i) { $bad = $bad + 1 }
    } catch ($e) { $bad = $bad + 1; $err = "" . $e }
}
db::exec($admin, "SET GLOBAL max_prepared_stmt_count = 16382", [])
print("bad=" . $bad . " " . $err)
LK
out=$(run full.lk); [ "$out" = "bad=0 " ] && ok "server limit set to 3 (other clients used it up): all 20 queries still run" || bad "server limit full: $out"

echo "-- transactions --"
cat > "$T/txn.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
db::begin($c)
db::exec($c, "INSERT INTO lk_shop_a.items VALUES (?, ?)", [900, "tx"])
$in = count(db::query($c, "SELECT v FROM lk_shop_a.items WHERE id = ?", [900]))
db::rollback($c)
$out = count(db::query($c, "SELECT v FROM lk_shop_a.items WHERE id = ?", [900]))
print($in . "|" . $out)
LK
out=$(run txn.lk); [ "$out" = "1|0" ] && ok "a rolled-back INSERT leaves nothing (seen inside, gone after)" || bad "transaction: $out"

echo "-- web: concurrent requests --"
cat > "$T/app.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
route("GET", "/r/{id}", function($id) use ($c) {
    $r = db::query($c, "SELECT v FROM lk_shop_b.items WHERE id = ?", [$id])
    return response::text($r[0]["v"])
})
route("GET", "/ok", fn() => response::text("up"))
LK
( cd "$T" && exec "$FCGI" --mode http --port 7761 app.lk > "$T/srv.log" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -m 1 -o /dev/null localhost:7761/ok && break; sleep 0.1; done
cbad=0; for round in 1 2 3 4; do
    cp=()   # yalnız istemcileri bekle — düz `wait` arka plandaki sunucuyu da beklerdi
    for i in $(seq 1 16); do id=$(( (round * 16 + i) % 50 + 1 )); ( r=$(curl -s -m 10 localhost:7761/r/$id); [ "$r" = "b$id" ] || echo x >> "$T/cbad" ) & cp+=($!); done
    wait "${cp[@]}"
done 2>/dev/null
[ -f "$T/cbad" ] && cbad=$(wc -l < "$T/cbad")
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
[ "$cbad" = 0 ] && ok "64 concurrent web requests on the pool: each got its own row" || bad "concurrent web requests: $cbad wrong"

[ $fail = 0 ] && echo "PASS: MySQL statement cache" || echo "FAIL: MySQL statement cache"
exit $fail

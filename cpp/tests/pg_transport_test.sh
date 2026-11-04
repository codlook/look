#!/usr/bin/env bash
# PostgreSQL: a query is one send, and replies are read through a buffer — results stay exact.
#
# A query (Parse, Bind, Describe, Execute, Sync) used to go out as ten send calls (each message's
# header, then its body) and every reply message was read with three recv calls (type, length,
# body). Now the five messages go in one send and replies are read through a 16 KB buffer. A
# buffer that keeps a byte too many, or too few, shifts every later reply — another query's rows —
# so this test covers each way it could:
#   - values far larger than the buffer (1 MB text), and results that span many buffers;
#   - a query that fails mid-way, then the next query on the same connection;
#   - a connection the server terminated: the new one starts with an empty buffer;
#   - 64 concurrent web requests each get their own row.
# Needs a live PostgreSQL server and a superuser-capable DSN (pg_terminate_backend).
# Usage: pg_transport_test.sh <lk> <lk-fcgi> <postgres-dsn>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
export DSN="${3:?usage}" LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
T="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }
run() { (cd "$T" && timeout 120 "$LK" "$1" 2>&1 | grep -v '^\[20'); }

cat > "$T/setup.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
db::exec($c, "DROP TABLE IF EXISTS lk_tr", [])
db::exec($c, "CREATE TABLE lk_tr (id INT PRIMARY KEY, v TEXT)", [])
for ($i = 1; $i <= 50; $i = $i + 1) { db::exec($c, "INSERT INTO lk_tr VALUES (?, ?)", [$i, "r" . $i]) }
print("setup ok")
LK
out=$(run setup.lk); [ "$out" = "setup ok" ] || { echo "FAIL: setup: $out"; exit 1; }

echo "-- sizes --"
cat > "$T/sizes.lk" <<'LK'
use db
use string
$c = db::connect(env("DSN"))
$big = db::query($c, "SELECT repeat('x', 1048576) AS b, 'after' AS t", [])
$many = db::query($c, "SELECT g AS n, md5(g::text) AS h FROM generate_series(1, 20000) g", [])
$sum = 0
foreach ($many as $r) { $sum = $sum + $r["n"] }
$next = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [7])
print(string::len($big[0]["b"]) . "|" . $big[0]["t"] . "|" . count($many) . "|" . $sum . "|" . $many[19999]["h"] . "|" . $next[0]["v"])
LK
want="1048576|after|20000|200010000|$(printf 20000 | md5sum | cut -d' ' -f1)|r7"
out=$(run sizes.lk); [ "$out" = "$want" ] && ok "a 1 MB value and a 20000-row result (many buffers), then the next query: exact" \
    || bad "sizes: got '$out' want '$want'"

echo "-- errors --"
cat > "$T/errors.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
$out = ""
for ($i = 1; $i <= 20; $i = $i + 1) {
    try { db::query($c, "SELECT nope FROM lk_tr WHERE id = ?", [$i]); $out = $out . "?" } catch ($e) { $out = $out . "e" }
    $r = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [$i])
    if ($r[0]["v"] != "r" . $i) { $out = $out . "X" }
}
print($out)
LK
out=$(run errors.lk); [ "$out" = "eeeeeeeeeeeeeeeeeeee" ] && ok "20 failing queries, each followed by a good one on the same connection: every good one exact" || bad "errors: $out"

echo "-- a connection the server terminated --"
cat > "$T/term.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
$k = db::connect(env("DSN") . "?application_name=lk_killer")
$a = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [3])[0]["v"]
$pid = db::query($c, "SELECT pg_backend_pid() AS p", [])[0]["p"]
db::query($k, "SELECT pg_terminate_backend(?)", [$pid])
$b = ""
try { $b = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [4])[0]["v"] } catch ($e) { $b = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [4])[0]["v"] }
$pid2 = db::query($c, "SELECT pg_backend_pid() AS p", [])[0]["p"]
print($a . "|" . $b . "|" . ($pid2 != $pid ? "new-connection" : "same-connection"))
LK
out=$(run term.lk); [ "$out" = "r3|r4|new-connection" ] && ok "terminated connection: replaced, and the new one reads only its own replies" || bad "after terminate: $out"

echo "-- web: concurrent requests --"
cat > "$T/app.lk" <<'LK'
use db
$c = db::connect(env("DSN"))
route("GET", "/r/{id}", function($id) use ($c) {
    $r = db::query($c, "SELECT v FROM lk_tr WHERE id = ?", [$id])
    return response::text($r[0]["v"])
})
route("GET", "/ok", fn() => response::text("up"))
LK
( cd "$T" && exec "$FCGI" --mode http --port 7762 app.lk > "$T/srv.log" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -m 1 -o /dev/null localhost:7762/ok && break; sleep 0.1; done
for round in 1 2 3 4; do
    cp=()   # yalnız istemcileri bekle — düz `wait` arka plandaki sunucuyu da beklerdi
    for i in $(seq 1 16); do id=$(( (round * 16 + i) % 50 + 1 )); ( r=$(curl -s -m 10 localhost:7762/r/$id); [ "$r" = "r$id" ] || echo x >> "$T/cbad" ) & cp+=($!); done
    wait "${cp[@]}"
done 2>/dev/null
cbad=0; [ -f "$T/cbad" ] && cbad=$(wc -l < "$T/cbad")
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
[ "$cbad" = 0 ] && ok "64 concurrent web requests on the pool: each got its own row" || bad "concurrent web requests: $cbad wrong"

[ $fail = 0 ] && echo "PASS: PostgreSQL transport" || echo "FAIL: PostgreSQL transport"
exit $fail

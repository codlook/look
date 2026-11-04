#!/usr/bin/env bash
# SQLite: a statement is prepared once per connection, and the cache never changes a result or
# holds the database.
#
# db::query / db::exec parsed and prepared the SQL on every call. Now each connection keeps its
# prepared statements; a statement is taken out of the cache while it runs and reset as soon as
# its rows are read. A cached statement that was not reset would keep its read transaction open:
# in WAL mode the checkpoint can then never finish and the file grows. This test covers:
#   - a table that changes shape, on the same connection and from another process;
#   - after thousands of cached reads in a running server, another process writes and runs a full
#     checkpoint — nothing may still be reading;
#   - errors stay errors (UNIQUE violation, wrong parameter count) and the next query works;
#   - 300 different statements (more than the cache holds);
#   - 64 concurrent web requests each get their own row.
# Usage: sqlite_stmt_cache_test.sh <lk> <lk-fcgi>
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage}")" && pwd)/$(basename "$2")"
T="$(mktemp -d)"; pid=""
export DBF="$T/cache.db" LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
fail=0; ok() { echo "  OK   $1"; }; bad() { echo "  FAIL $1"; fail=1; }
run() { (cd "$T" && timeout 120 "$LK" "$1" 2>&1 | grep -v '^\[20'); }

cat > "$T/setup.lk" <<'LK'
use db
$c = db::connect("sqlite://" . env("DBF"))
db::exec($c, "CREATE TABLE items (id INTEGER PRIMARY KEY, v TEXT UNIQUE)", [])
for ($i = 1; $i <= 50; $i = $i + 1) { db::exec($c, "INSERT INTO items VALUES (?, ?)", [$i, "r" . $i]) }
print("setup ok")
LK
out=$(run setup.lk); [ "$out" = "setup ok" ] || { echo "FAIL: setup: $out"; exit 1; }

echo "-- one connection --"
cat > "$T/shape.lk" <<'LK'
use db
use json
$c = db::connect("sqlite://" . env("DBF"))
$q = "SELECT * FROM items WHERE id = ?"
$a = json::encode(db::query($c, $q, [1]))
db::exec($c, "ALTER TABLE items ADD COLUMN price INTEGER DEFAULT 7", [])
$b = json::encode(db::query($c, $q, [1]))
$e1 = ""; try { db::exec($c, "INSERT INTO items (id, v) VALUES (?, ?)", [99, "r1"]) } catch ($e) { $e1 = "unique" }
$e2 = ""; try { db::exec($c, "INSERT INTO items (id, v) VALUES (?, ?)", [98, "x1"]) } catch ($e) { $e2 = "failed" }
$e3 = ""; try { db::query($c, $q, [1, 2]) } catch ($e) { $e3 = "count" }
$c2 = json::encode(db::query($c, $q, [98]))
$n = 0
for ($i = 1; $i <= 300; $i = $i + 1) {
    $r = db::query($c, "SELECT " . $i . " AS n FROM items WHERE id = ?", [1])
    if ($r[0]["n"] == $i) { $n = $n + 1 }
}
print($a)
print($b)
print($e1 . "|" . $e2 . "|" . $e3 . "|" . $c2 . "|" . $n)
LK
want='[{"id":1,"v":"r1"}]
[{"id":1,"v":"r1","price":7}]
unique||count|[{"id":98,"v":"x1","price":7}]|300'
out=$(run shape.lk); [ "$out" = "$want" ] && ok "ALTER on the same connection, a UNIQUE violation, a wrong parameter count, then 300 different statements: all exact" \
    || { bad "one connection"; echo "$out" | sed 's/^/      got: /'; }

echo "-- a running server and another process --"
cat > "$T/app.lk" <<'LK'
use db
use json
$c = db::connect("sqlite://" . env("DBF"))
route("GET", "/r/{id}", function($id) use ($c) {
    $r = db::query($c, "SELECT * FROM items WHERE id = ?", [$id])
    return response::text(json::encode($r[0]))
})
route("GET", "/ok", fn() => response::text("up"))
LK
( cd "$T" && exec "$FCGI" --mode http --port 7763 app.lk > "$T/srv.log" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -m 1 -o /dev/null localhost:7763/ok && break; sleep 0.1; done
for i in $(seq 1 2000); do echo "url = \"http://127.0.0.1:7763/r/$(( i % 50 + 1 ))\""; done > "$T/urls"
curl -s -K "$T/urls" --max-time 60 > /dev/null   # -o yalnız ilk adrese uygulanırdı
cat > "$T/writer.lk" <<'LK'
use db
$w = db::connect("sqlite://" . env("DBF"))
db::exec($w, "ALTER TABLE items ADD COLUMN color TEXT DEFAULT 'red'", [])
db::exec($w, "UPDATE items SET v = ? WHERE id = ?", ["changed", 5])
$cp = db::query($w, "PRAGMA wal_checkpoint(TRUNCATE)", [])
print("busy=" . $cp[0]["busy"])
LK
out=$(run writer.lk); [ "$out" = "busy=0" ] && ok "after 2000 cached reads in the server, another process writes and a full checkpoint finishes (no read left open)" \
    || bad "checkpoint after cached reads: $out"
r=$(curl -s -m 5 localhost:7763/r/5)
[ "$r" = '{"id":5,"v":"changed","price":7,"color":"red"}' ] && ok "the server's cached statement sees the other process's new column and row" || bad "after the other process changed the table: $r"
for round in 1 2 3 4; do
    cp=()   # yalnız istemcileri bekle — düz `wait` arka plandaki sunucuyu da beklerdi
    for i in $(seq 1 16); do id=$(( (round * 16 + i) % 50 + 1 ))
        ( r=$(curl -s -m 10 localhost:7763/r/$id); echo "$r" | grep -q "\"id\":$id," || echo x >> "$T/cbad" ) & cp+=($!); done
    wait "${cp[@]}"
done 2>/dev/null
cbad=0; [ -f "$T/cbad" ] && cbad=$(wc -l < "$T/cbad")
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
[ "$cbad" = 0 ] && ok "64 concurrent web requests: each got its own row" || bad "concurrent web requests: $cbad wrong"

[ $fail = 0 ] && echo "PASS: SQLite statement cache" || echo "FAIL: SQLite statement cache"
exit $fail

#!/usr/bin/env bash
# A request that ends inside an open transaction does not hand that transaction to the next request.
#
# release_thread_connections() gave the connection back to the pool as it was. After db::begin and
# an error (or a forgotten commit), the next request on that connection ran INSIDE the open
# transaction: its own writes were never committed (another connection did not see them), and a
# later commit made the failed request's writes permanent. With SQLite the write gate stayed held
# and every other writer failed after 10 s. Measured on MySQL, PostgreSQL and SQLite alike. Now the
# open transaction is rolled back when the request ends (or the connection is closed if that fails),
# and the log says so.
# The app also runs a query at the top level with a pool of one connection: setup used to keep
# that connection for the life of the process, and every later database request waited forever.
# Usage: db_open_transaction_release_test.sh <lk-fcgi> <lk>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> <lk>}")" && pwd)/$(basename "$1")"
LK="$(cd "$(dirname "${2:?usage: $0 <lk-fcgi> <lk>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7748}"
cat > "$TMP/app.lk" <<LK
\$db = db::connect("sqlite://$TMP/t.db")
db::exec(\$db, "CREATE TABLE IF NOT EXISTS t (who TEXT)")
route("GET", "/bad", function() use (\$db) {
    db::begin(\$db)
    db::exec(\$db, "INSERT INTO t (who) VALUES ('bad')")
    throw "boom"
})
route("GET", "/forgot", function() use (\$db) {
    db::exec(\$db, "BEGIN")
    db::exec(\$db, "INSERT INTO t (who) VALUES ('forgot')")
    return response::text("left open")
})
route("GET", "/good", function() use (\$db) { db::exec(\$db, "INSERT INTO t (who) VALUES ('good')"); return response::text("ok") })
route("GET", "/tx", function() use (\$db) {
    db::transaction(\$db, function() use (\$db) { db::exec(\$db, "INSERT INTO t (who) VALUES ('tx')") })
    return response::text("ok")
})
LK
cat > "$TMP/read.lk" <<LK
\$db = db::connect("sqlite://$TMP/t.db")
print(json::encode(db::query(\$db, "SELECT who FROM t ORDER BY rowid")))
LK
fail=0
( cd "$TMP" && exec env LOOK_DB_POOL_SIZE=1 "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/good" && break; sleep 0.1; done
curl -s -o /dev/null -m 15 "localhost:$PORT/bad"
curl -s -o /dev/null -m 15 "localhost:$PORT/forgot"
t0=$(date +%s%N)
curl -s -o /dev/null -m 30 "localhost:$PORT/good"
curl -s -o /dev/null -m 30 "localhost:$PORT/tx"
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
# another process, another connection: only committed rows are visible
got="$(timeout 20 "$LK" "$TMP/read.lk" 2>/dev/null | tail -1)"
want='[{"who":"good"},{"who":"good"},{"who":"tx"}]'
[ "$got" = "$want" ] && echo "  OK   another connection sees only committed work: $got" \
    || { echo "  FAIL another connection sees $got (want $want)"; fail=1; }
[ $ms -lt 5000 ] && echo "  OK   the next requests were not held by a dangling write lock (${ms} ms)" \
    || { echo "  FAIL the next requests took ${ms} ms"; fail=1; }
[ "$(grep -c 'a request ended inside an open transaction' "$TMP/log.txt")" -ge 2 ] && echo "  OK   the log names both requests that left a transaction open" \
    || { echo "  FAIL no log line for the open transaction"; fail=1; }
kill -0 "$pid" 2>/dev/null && echo "  OK   the server is still running" || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: an open transaction is rolled back when its request ends" \
             || { echo "FAIL: open transaction handed to the next request"; exit 1; }

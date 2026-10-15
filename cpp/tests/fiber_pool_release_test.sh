#!/usr/bin/env bash
# Concurrent requests on fibers give their database connection back to the pool.
#
# With fibers, one worker thread runs many requests: a request that waits (here for the SQLite
# write) is suspended and the next one starts on the same thread. The dispatch depth that tells
# a nested dispatch (a job handler run inside a request) from a new request was per thread, so
# every request started while another one was suspended on that thread counted as "nested":
# it did not return its connection, the pool (4 for SQLite) ran out and the remaining requests
# were suspended forever — 19-28 of 40 concurrent writes got no response at all.
# This test sends concurrent writes in both dispatch modes and checks that every one is
# answered, every row is written and no connection is left checked out afterwards (pool use equals the idle value).
# Usage: fiber_pool_release_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7831}"
cat > "$TMP/app.lk" <<'LK'
use runtime
function conn() { return db::connect("sqlite://w.db") }
$c0 = conn()
db::exec($c0, "CREATE TABLE IF NOT EXISTS hits (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT)")
route("GET", "/w", function() { $c = conn(); db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["w"]); return response::text("ok") })
route("GET", "/r", function() { $r = db::query(conn(), "SELECT COUNT(*) AS c FROM hits"); return response::text("n=" . $r[0]["c"]) })
route("GET", "/busy", function() { $s = runtime::stats(); return response::text("busy=" . $s["db_pool_busy"]) })
LK
fail=0
for mode in 1 0; do
    rm -f "$TMP"/w.db* "$TMP"/codes.txt
    ( cd "$TMP" && LOOK_FIBER_DISPATCH=$mode exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/busy" && break; sleep 0.1; done
    busy0="$(curl -s -m 10 "localhost:$PORT/busy")"   # kurulum bağlantısı + bu isteğin kendisi
    for p in $(seq 1 16); do
        ( for j in $(seq 1 10); do curl -s -m 10 -o /dev/null -w '%{http_code}\n' "localhost:$PORT/w"; done >> "$TMP/codes.txt" ) &
    done
    wait $(jobs -p | grep -v "^$pid\$") 2>/dev/null
    ok="$(grep -c '^200$' "$TMP/codes.txt")"
    rows="$(curl -s -m 10 "localhost:$PORT/r")"
    busy="$(curl -s -m 10 "localhost:$PORT/busy")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$ok" = 160 ] && [ "$rows" = "n=160" ] && [ "$busy" = "$busy0" ]; then
        echo "  OK   LOOK_FIBER_DISPATCH=$mode: 160/160 concurrent writes answered, $rows, $busy as before the load"
    else
        echo "  FAIL LOOK_FIBER_DISPATCH=$mode: $ok/160 answered, [$rows], [$busy] (want n=160, $busy0 as before the load)"; fail=1
    fi
done
[ $fail = 0 ] && echo "PASS: fiber requests return their DB connection" || { echo "FAIL: fiber requests return their DB connection"; exit 1; }

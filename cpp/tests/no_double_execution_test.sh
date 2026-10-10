#!/usr/bin/env bash
# A route handler runs ONCE per request, also when it fails.
#
# Until 1.0.8 the web path treated every error raised while the VM ran a handler — a plain
# `throw`, a database error, an undefined variable — as a VM fault and ran the SAME request
# again on the tree-walk engine. Whatever the handler had done before the error happened
# twice in one request (measured: "INSERT, then fail" wrote 2 rows), and the route stayed on
# the slow engine until restart. With LOOK_VM_STRICT=1 the error escaped the worker instead:
# its connections were never released and after as many errors as there are workers the
# server stopped answering.
# Now a run-time error is a 500, once, on every engine and in every mode.
# Usage: no_double_execution_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7718}"
cat > "$TMP/app.lk" <<LK
use json
use jobs
function conn() { return db::connect("sqlite://$TMP/t.db") }
\$c0 = conn()
db::exec(\$c0, "CREATE TABLE IF NOT EXISTS hits (n INTEGER)")
function hit() { db::exec(conn(), "INSERT INTO hits (n) VALUES (1)") }
route("GET", "/dberr", function() { hit(); db::query(conn(), "SELECT * FROM nope"); return "x" })
route("GET", "/throw", function() { hit(); throw "boom" })
route("GET", "/undef", function() { hit(); return \$undefined_thing })
route("GET", "/n", fn() => response::json(["hits" => count(db::query(conn(), "SELECT n FROM hits"))]))
LK
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    for u in dberr throw undef; do
        rm -f "$TMP/t.db"
        ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/n" && break; sleep 0.1; done
        c1="$(curl -s -m 4 -o /dev/null -w '%{http_code}' "localhost:$PORT/$u")"; n1="$(curl -s -m 4 "localhost:$PORT/n")"
        # more failing requests than there are workers: the server must keep answering
        for i in 1 2 3 4 5; do curl -s -m 4 -o /dev/null "localhost:$PORT/$u"; done
        n6="$(curl -s -m 4 "localhost:$PORT/n")"
        moved="$(grep -c 'to the interpreter\|interpreter fallback' "$TMP/log.txt")"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        if [ "$c1" = "500" ] && [ "$n1" = '{"hits":1}' ] && [ "$n6" = '{"hits":6}' ] && [ "$moved" = "0" ]; then
            echo "  OK   [$mode] /$u: 500, the handler ran once per request, the server kept answering"
        else echo "  FAIL [$mode] /$u: status=$c1 after-1=$n1 after-6=$n6 moved-to-interpreter=$moved"; fail=1; fi
    done
done

[ $fail = 0 ] && echo "PASS: a failing handler runs once and does not take a worker down" || { echo "FAIL: handler ran more than once or the server stopped answering"; exit 1; }

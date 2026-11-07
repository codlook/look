#!/usr/bin/env bash
# A failing route answers 500 with nothing from the failure in the body — no exception text, no
# file path, no value — in every mode (plain, LOOK_DEBUG=1, JSON client), fiber and pool. The
# error goes to the server log, where a database password from the DSN must not appear either.
# Usage: error_leak_test.sh <lk-fcgi> [port]
set -u
FCGI="$(cd "$(dirname "${1:?usage: error_leak_test.sh <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7906}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("GET", "/throw", function() { throw "LEAKMARK-secret at /var/www/app/LEAKPATH.lk" })
route("GET", "/div",   function() { $x = 0; return response::text(1 / $x) })
route("GET", "/undef", function() { return response::text($LEAKVAR) })
route("GET", "/db",    function() { $c = db::connect("mysql://u:LEAKPASS@127.0.0.1:1/d"); return "x" })
route("GET", "/ok",    fn() => response::text("ok"))
LK
fail=0
for mode in plain debug pool; do
    envs="LOOK_SECRET=leak-test-secret-0123456789abcdef0123456789"
    [ $mode = debug ] && envs="$envs LOOK_DEBUG=1"
    [ $mode = pool ] && envs="$envs LOOK_FIBER_DISPATCH=0"
    ( cd "$T" && exec env $envs "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log_$mode.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    for r in throw div undef db; do
        for acc in "text/html" "application/json"; do
            body="$(curl -s -m 10 -H "Accept: $acc" -w '|%{http_code}' "localhost:$PORT/$r")"
            code="${body##*|}"; body="${body%|*}"
            if [ "$code" = 500 ] && ! echo "$body" | grep -qE 'LEAK|/var/www|Undefined|division|stoi|mysql://'; then :
            else echo "  FAIL [$mode] /$r ($acc): HTTP $code, body: $(echo "$body" | head -c 200)"; fail=1; fi
        done
    done
    kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
    grep -q "LEAKMARK" "$T/log_$mode.txt" || { echo "  FAIL [$mode] the error did not reach the server log"; fail=1; }
    grep -q "LEAKPASS" "$T/log_$mode.txt" && { echo "  FAIL [$mode] the DSN password is in the server log"; fail=1; }
    [ $fail = 0 ] && echo "  OK   [$mode] 500 bodies carry nothing from the error; log has it, without the password"
done
[ $fail = 0 ] && echo "PASS: error answers do not leak" || { echo "FAIL: error answers leak"; exit 1; }

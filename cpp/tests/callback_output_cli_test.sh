#!/usr/bin/env bash
# On the command line, the output of a job handler, a timer callback and a parallel() task reaches
# stdout. The tree-walk engine wrote it to a buffer of its own and threw it away, so `print` in a
# job handler of a command-line worker script was silently lost (the VM printed parallel() output,
# the tree-walk engine did not).
# In a web application nothing changes: a callback has no response to write to, so a handler that
# a route runs with jobs::run does not write into that route's response.
# Usage: callback_output_cli_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7755}"
fail=0
cat > "$TMP/cb.lk" <<'LK'
use jobs
use cache
jobs::worker("mail", function($job) {
    $p = $job["payload"]
    print("handled {$p}")
    return true
})
jobs::push("mail", "a@x")
jobs::run(0)
timer::after(30, function() { print("timer ran"); cache::set("t", 1, 60) })
$i = 0
while ($i < 50000000 && cache::get("t") == null) { $i = $i + 1 }
$c = channel()
parallel(function() use ($c) { print("from task"); send($c, 1) })
$v = receive($c)
print("main done")
LK
for eng in LOOK_X=1; do   # LOOK 2: komut satırı yalnız VM
    rm -f "$TMP"/*.db*
    out="$(cd "$TMP" && env $eng "$LK" cb.lk 2>/dev/null)"
    miss=""
    for want in "handled a@x" "timer ran" "from task" "main done"; do
        echo "$out" | grep -qx "$want" || miss="$miss [$want]"
    done
    [ -z "$miss" ] && echo "  OK   [$eng] job handler, timer and parallel() output reach stdout" \
                   || { echo "  FAIL [$eng] missing:$miss out=[$out]"; fail=1; }
done

cat > "$TMP/web.lk" <<'LK'
use jobs
jobs::worker("w", function($job) { print("LEAK"); return true })
route("GET", "/run", function() { jobs::push("w", "x"); jobs::run(0); return response::text("route body") })
LK
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 2 web.lk > "$TMP/web.log" 2>&1 ) & pid=$!
for i in $(seq 1 60); do curl -s -o /dev/null -m 1 "localhost:$PORT/run" && break; sleep 0.05; done
body="$(curl -s -m 5 "localhost:$PORT/run")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
[ "$body" = "route body" ] && echo "  OK   web: a handler run by a route does not write into the route's response" \
                           || { echo "  FAIL web: body=[$body]"; fail=1; }

[ $fail = 0 ] && echo "PASS: callback output reaches stdout on the command line" || { echo "FAIL: callback output"; exit 1; }

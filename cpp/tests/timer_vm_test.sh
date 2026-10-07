#!/usr/bin/env bash
# LOOK 2: a timer armed in a route runs on the VM.
#
# timer:: used to exist only inside the tree-walk engine. A route that armed a timer could
# not run on the VM at all: until 1.0.7 it worked through a second run of the request, since
# 1.0.8 the server placed such a route on the tree-walk engine at startup. Now timer::after,
# timer::every and timer::cancel are VM builtins and the callback runs like a new request:
# its own VM, the declared values of the globals, the values it captured.
#   * the route stays on the VM, also with LOOK_VM_STRICT=1 (nothing is placed on the
#     tree-walk engine, nothing falls back)
#   * the callback runs, with the value it captured
#   * what the callback writes to a global reaches no later request
#   * timer::every repeats until timer::cancel
#   * an error in a callback is logged and the server keeps answering
# Usage: timer_vm_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7722}"
cat > "$TMP/app.lk" <<'LK'
use json
use cache
const { LIST = ["c0"] }
$G = ["g0"]
function mark($who) { cache::set("ran_" . $who, "yes", 60); $G[0] = $who; return 1 }
function ran($who) { return cache::get("ran_" . $who) ?? "no" }
function tick() { cache::set("ticks", (cache::get("ticks") ?? 0) + 1, 60); return 1 }
function arm() { timer::after(50, fn() => mark("NAMED")); return 1 }
route("GET", "/after", function() {
    $v = request::get("v")
    timer::after(50, fn() => cache::set("captured", $v, 60))
    timer::after(50, fn() => mark("DIRECT"))
    return response::json(arm())
})
route("GET", "/every", function() {
    $id = timer::every(40, fn() => tick())
    cache::set("id", $id, 60)
    return response::json(1)
})
route("GET", "/cancel", function() { timer::cancel(cache::get("id")); return response::json(1) })
route("GET", "/boom", function() { timer::after(20, function() { throw "boom in timer" }); return response::json(1) })
route("GET", "/r", fn() => response::json(["global" => $G[0], "const" => LIST[0], "direct" => ran("DIRECT"),
      "named" => ran("NAMED"), "captured" => cache::get("captured") ?? "none"]))
route("GET", "/ticks", fn() => response::json(cache::get("ticks") ?? 0))
LK
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    a="$(curl -s -m 3 "localhost:$PORT/after?v=cap42")"; sleep 0.5
    r="$(curl -s -m 3 "localhost:$PORT/r")"
    curl -s -m 3 "localhost:$PORT/every" >/dev/null; sleep 0.5
    t1="$(curl -s -m 3 "localhost:$PORT/ticks")"; curl -s -m 3 "localhost:$PORT/cancel" >/dev/null; sleep 0.2
    t2="$(curl -s -m 3 "localhost:$PORT/ticks")"; sleep 0.4; t3="$(curl -s -m 3 "localhost:$PORT/ticks")"
    curl -s -m 3 "localhost:$PORT/boom" >/dev/null; sleep 0.3
    alive="$(curl -s -m 3 "localhost:$PORT/ticks")"
    placed="$(grep -c 'runs on the interpreter\|VM BUG\|interpreter fallback' "$TMP/log.txt")"
    boomlog="$(grep -c 'boom in timer' "$TMP/log.txt")"
    vmroutes="$(grep -o 'VM routes: [0-9]* registered' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    WANT='{"global":"g0","const":"c0","direct":"yes","named":"yes","captured":"cap42"}'
    if [ "$a" = "1" ] && [ "$r" = "$WANT" ]; then echo "  OK   [$mode] callbacks ran with the captured value; the global is untouched for later requests"
    else echo "  FAIL [$mode] arm=$a r=$r"; fail=1; fi
    if [ "$t1" -ge 5 ] 2>/dev/null && [ "$t2" = "$t3" ]; then echo "  OK   [$mode] timer::every repeated ($t1 ticks in 0.5 s) and stopped after timer::cancel"
    else echo "  FAIL [$mode] every/cancel: ticks=$t1 after-cancel=$t2 later=$t3"; fail=1; fi
    if [ "$placed" = "0" ] && [ "$vmroutes" = "VM routes: 6 registered" ]; then echo "  OK   [$mode] all 6 routes on the VM, none placed on the tree-walk engine, no fallback"
    else echo "  FAIL [$mode] routes: '$vmroutes', interpreter/fallback log lines: $placed"; fail=1; fi
    if [ "$boomlog" -ge 1 ] && [ -n "$alive" ]; then echo "  OK   [$mode] an error in a callback is logged and the server keeps answering"
    else echo "  FAIL [$mode] callback error: logged=$boomlog alive=[$alive]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: timers armed in a route run on the VM" || { echo "FAIL: timers on the VM"; exit 1; }

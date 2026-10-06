#!/usr/bin/env bash
# A timer must fire when it is due, not when the timer thread happens to wake up.
#
# The timer thread computed how long to sleep BEFORE it went to sleep. A timer added while it
# slept woke it, but the thread saw "not due yet" and went back to sleep until the OLD
# deadline — 30 seconds when no other timer was pending. Only a timer armed before the thread
# had started waiting (in practice: the first one in the process) fired on time.
# Here three timers of 50 ms are armed one second apart; each must have run within half a
# second of being armed. Fixed in 1.0.8.
# Usage: timer_late_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7720}"
cat > "$TMP/app.lk" <<'LK'
use json
use cache
function mark($k) { cache::set("ran_" . $k, "yes", 60); return 1 }
route("GET", "/arm/{k}", function() { $k = request::param("k"); timer::after(50, fn() => mark($k)); return response::json(1) })
route("GET", "/ran/{k}", fn() => response::json(cache::get("ran_" . request::param("k")) ?? "no"))
LK
fail=0
for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ran/x" && break; sleep 0.1; done
    got=""
    for k in a b c; do
        curl -s -m 3 "localhost:$PORT/arm/$k" >/dev/null; sleep 0.5
        got="$got$(curl -s -m 3 "localhost:$PORT/ran/$k")"; sleep 0.5
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = '"yes""yes""yes"' ]; then echo "  OK   [$mode] three timers armed one second apart each fired on time"
    else echo "  FAIL [$mode] fired within 0.5 s: $got (want \"yes\" three times)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: timers fire when they are due" || { echo "FAIL: a timer fired late or not at all"; exit 1; }

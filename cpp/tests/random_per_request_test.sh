#!/usr/bin/env bash
# math::random gives different numbers to different requests.
#
# The generator was seeded again with the current second every time a copy of the standard
# library was built: for a worker's first request, and for every job handler and timer
# callback on the tree-walk engine. Each re-seed restarts the sequence. Measured on 1.0.12:
# the first two requests after a start return the same number (312643972, 312643972), and
# everything built in the same second starts from the same value. It is seeded once per
# process now.
# The server is probed on another path first, so the numbers below are the first ones the
# route ever produces.
# Usage: random_per_request_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7755}"
cat > "$TMP/app.lk" <<'LK'
use math
route("GET", "/ready", fn() => response::text("ok"))
route("GET", "/r", fn() => response::text(math::random(1, 1000000000)))
LK
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    for workers in 1 4; do
        ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers $workers app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
        for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ready" && break; sleep 0.1; done
        : > "$TMP/v.txt"; for i in $(seq 1 40); do curl -s -m 3 "localhost:$PORT/r" >> "$TMP/v.txt"; echo >> "$TMP/v.txt"; done
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
        distinct="$(sort -u "$TMP/v.txt" | grep -c .)"
        if [ "$distinct" = "40" ]; then echo "  OK   [$mode, $workers worker(s)] the first 40 requests: 40 different numbers"
        else echo "  FAIL [$mode, $workers worker(s)] the first 40 requests gave only $distinct different numbers (first: $(head -3 "$TMP/v.txt" | tr '\n' ' '))"; fail=1; fi
    done
done
[ $fail = 0 ] && echo "PASS: math::random differs between requests" || { echo "FAIL: math::random repeats"; exit 1; }

#!/usr/bin/env bash
# math::random gives different numbers to different requests, worker threads and server
# starts.
#
# It used the C generator, seeded again with the current second every time a copy of the
# standard library was built. Threads created in the same second produced the same sequence,
# and the sequence followed from the second the process started. Measured on 1.0.13: 16
# requests at once returned 4 different numbers, one of them 8 times; the first requests of
# each worker were equal; two servers started in the same second gave the same first number.
# Every worker thread now has its own generator, seeded from the operating system's secure
# source. (It is still not a generator for secrets: codes, keys and passwords are made with
# crypto::random_string / string::random.)
# The server is probed on another path first, so the numbers are the first the route produces.
# Usage: random_per_request_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7755}"
cat > "$TMP/app.lk" <<'LK'
use math
route("GET", "/ready", fn() => response::text("ok"))
route("GET", "/r", fn() => response::text(math::random(1, 1000000000)))
route("GET", "/f", fn() => response::text(math::random()))
LK
start() { ( cd "$TMP" && exec env $1 "$FCGI" --mode http --port "$PORT" --workers $2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
          for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ready" && break; sleep 0.1; done; }
stop()  { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    # 1) 16 requests at once on 4 workers, as the first thing the server does
    start "$mode" 4
    pids=""; for i in $(seq 1 16); do curl -s -m 5 -o "$TMP/c$i.txt" "localhost:$PORT/r" & pids="$pids $!"; done; wait $pids
    all="$(for i in $(seq 1 16); do cat "$TMP/c$i.txt"; echo; done | grep -c .)"
    conc="$(for i in $(seq 1 16); do cat "$TMP/c$i.txt"; echo; done | grep . | sort -u | grep -c .)"
    # 2) 40 more in a row; none may repeat, and the range is really used
    : > "$TMP/v.txt"; for i in $(seq 1 40); do curl -s -m 3 "localhost:$PORT/r" >> "$TMP/v.txt"; echo >> "$TMP/v.txt"; done
    seq_distinct="$(sort -u "$TMP/v.txt" | grep -c .)"; big="$(awk '$1 > 32767' "$TMP/v.txt" | grep -c .)"
    f="$(curl -s -m 3 "localhost:$PORT/f")"
    first_a="$(head -1 "$TMP/c1.txt")"
    stop
    # 3) a second server right away: its first number is another one
    start "$mode" 1; first_b="$(curl -s -m 3 "localhost:$PORT/r")"; stop
    start "$mode" 1; first_c="$(curl -s -m 3 "localhost:$PORT/r")"; stop
    case "$f" in 0.*|[0-9]*e-*) fok=1;; *) fok=0;; esac
    if [ "$all" = "16" ] && [ "$conc" = "16" ] && [ "$seq_distinct" = "40" ] && [ "$big" -ge 38 ] && [ "$fok" = "1" ] && [ "$first_b" != "$first_c" ]; then
        echo "  OK   [$mode] 16 at once: 16 different; 40 in a row: 40 different; two starts differ; math::random() is in [0,1)"
    else echo "  FAIL [$mode] at-once distinct=$conc of $all; in-a-row distinct=$seq_distinct of 40; above-32767=$big; float=[$f]; first numbers of two starts: $first_b / $first_c"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: math::random differs between requests, threads and starts" || { echo "FAIL: math::random repeats"; exit 1; }

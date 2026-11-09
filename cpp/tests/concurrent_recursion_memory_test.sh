#!/usr/bin/env bash
# Many requests recursing without end at the same time must not exhaust the server's memory.
#
# fec55ef raised the call limit to 1M calls / a 256 MB call stack PER REQUEST. Each request alone
# ended in a clean error, but concurrent ones added up (measured: 12 at once took the server to
# 2.3 GB; the analyst saw 1.98 GB) — on a 2 GB server the kernel kills the whole process and every
# site on the binary goes down. Now: at most 10,000 nested calls and an 8 MB call stack per request
# (Node stops near 9k; a real program rarely goes deeper), so 32 such requests stay far below the
# memory of one of the old ones.
# The server's peak resident memory (VmHWM) is the measure, so the test needs no memory-limited
# machine and cannot take the CI runner down. Linux only (/proc); elsewhere it reports SKIP.
# Usage: concurrent_recursion_memory_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
[ -r /proc/self/status ] || { echo "SKIP: needs /proc (Linux)"; exit 0; }
command -v curl >/dev/null || { echo "SKIP: needs curl"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
cd "$TMP"
PORT="${PORT:-7825}"; N="${N:-32}"; LIMIT_MB="${LIMIT_MB:-400}"
cat > app.lk <<'LK'
function endless($n) { return endless($n + 1) + 1 }
function wide($n) { $a=1;$b=2;$c=3;$d=4;$e=5;$f=6;$g=7;$h=8;$i=9;$j=10;$k=11;$l=12;$m=13;$o=14;$q=15;$r=16
  return 1 + wide($n + 1) + $a+$b+$c+$d+$e+$f+$g+$h+$i+$j+$k+$l+$m+$o+$q+$r }
route("GET", "/endless", fn() => response::text("" . endless(0)))
route("GET", "/wide", fn() => response::text("" . wide(0)))
route("GET", "/ok", fn() => response::text("alive"))
LK
fail=0
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
  for ep in endless wide; do
    ( exec env $mode "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > log.txt 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    cp=""; for i in $(seq 1 "$N"); do curl -s -o "out.$i" -w '%{http_code}\n' -m 120 "localhost:$PORT/$ep" > "code.$i" & cp="$cp $!"; done
    wait $cp
    peak=$(awk '/VmHWM/ {print int($2 / 1024)}' "/proc/$pid/status" 2>/dev/null)
    up=no; [ "$(curl -s -m 5 "localhost:$PORT/ok")" = alive ] && up=yes
    c500=$(cat code.* | grep -c '^500$')
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    label="[${mode:-default}] $N x /$ep"
    if [ "$up" = yes ] && [ "${peak:-99999}" -le "$LIMIT_MB" ] && [ "$c500" = "$N" ]; then
      echo "  OK   $label: all $N a clean 500, server up, peak ${peak} MB (limit $LIMIT_MB)"
    else
      echo "  FAIL $label: up=$up peak=${peak:-?} MB (limit $LIMIT_MB) clean-500s=$c500/$N"; fail=1
    fi
    rm -f out.* code.*
  done
done
[ $fail = 0 ] && echo "PASS: concurrent endless recursion stays within memory, server up" || { echo "FAIL: concurrent recursion memory"; exit 1; }

#!/usr/bin/env bash
# Call depth: deep recursion works far past Node, endless recursion is a clean error, nothing crashes.
#
# Until now every call chain stopped at 500 (Node stops at about 9k plain / 2k through callbacks).
# Measured (2026-11-09): a LOOK call uses heap, not the C++ stack (~330 bytes per level), so the
# limit is memory: at most 1M nested calls and a 256 MB call stack. A call made from inside a
# builtin's callback (map/filter/sort/... calling a LOOK function) re-enters the VM on the C++ stack
# (~1.5 KB per level; 8 MB held 5,000 and crashed at 6,000), so it has its own limit, 4,000, and
# below it a check of the real remaining stack: with a small stack (ulimit -s, small threads) the
# chain ends in a clean error before the stack is full. Node and Go kill the whole server when a
# request recurses too deep (measured); here the request gets the error and the server stays up.
# Usage: call_depth_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
cd "$TMP"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

cat > lib.lk <<'LK'
use array
function p($n) { if ($n <= 0) { return 0 } return 1 + p($n - 1) }
function endless($n) { return endless($n + 1) + 1 }
function wide($n) { $a=1;$b=2;$c=3;$d=4;$e=5;$f=6;$g=7;$h=8;$i=9;$j=10;$k=11;$l=12;$m=13;$o=14;$q=15;$r=16
  if ($n <= 0) { return 0 } return 1 + wide($n - 1) + $a+$b+$c+$d+$e+$f+$g+$h+$i+$j+$k+$l+$m+$o+$q+$r - 136 }
function cb($n) { if ($n <= 0) { return 0 } return array::map([1], fn($x) => cb($n - 1))[0] + 1 }
function cb_endless($n) { return array::map([1], fn($x) => cb_endless($n + 1))[0] }
function try_it($f) { try { return "ok:" . $f() } catch ($e) { return "" . $e } }
LK
# case: label | expression | expected substring
run() {
    printf 'use "lib.lk"\nprint(try_it(fn() => %s))\nprint("after")\n' "$2" > t.lk
    out=$(timeout 120 "$LK" t.lk 2>&1); rc=$?
    if [ $rc -ge 124 ]; then bad "$1: process died or hung (exit $rc)"; return; fi
    case "$out" in *"$3"*after*) ok "$1";; *) bad "$1: got [$(echo "$out" | tail -c 220 | tr '\n' ' ')], want [$3]";; esac
}
run "plain recursion 100,000 levels"     'p(100000)'    "ok:100000"
run "plain recursion 700,000 levels"     'p(700000)'    "ok:700000"
run "endless recursion: clean error"     'endless(0)'   "Stack overflow: more than 1000000 nested calls"
run "call stack over 256 MB: clean error" 'wide(1000000)' "Stack overflow: the call stack is larger than 256 MB"
run "callbacks 3,999 levels"             'cb(3999)'     "ok:3999"
run "callbacks 4,001 levels: clean error" 'cb(4001)'    "Stack overflow: more than 4000 nested callbacks"
run "endless callbacks: clean error"     'cb_endless(0)' "Stack overflow: more than 4000 nested callbacks"

# Small stacks: the real-stack check answers before the stack is full; ordinary callbacks still work.
printf 'use "lib.lk"\nprint(try_it(fn() => cb_endless(0)))\nprint(array::map([1, 2, 3], fn($x) => $x * 2)[2])\nprint(try_it(fn() => cb(100)))\n' > small.lk
for s in 2048 512 128; do
    out=$( (ulimit -s $s; timeout 60 "$LK" small.lk) 2>&1 ); rc=$?
    if [ $rc -ge 124 ]; then bad "ulimit -s ${s}KB: process died (exit $rc)"; continue; fi
    l1=$(echo "$out" | sed -n 1p); l2=$(echo "$out" | sed -n 2p); l3=$(echo "$out" | sed -n 3p)
    case "$l1" in *"Stack overflow"*) ;; *) bad "ulimit -s ${s}KB: endless callbacks gave [$l1]"; continue;; esac
    [ "$l2" = "6" ] || { bad "ulimit -s ${s}KB: an ordinary map broke: [$l2]"; continue; }
    case "$l3" in "ok:100"|*"the native stack is nearly full"*) ok "ulimit -s ${s}KB: clean error, ordinary callbacks work ($l3)";;
        *) bad "ulimit -s ${s}KB: 100 callbacks gave [$l3]";; esac
done

# Server: endless recursion (plain and through callbacks) is a 500 and the server stays up.
PORT="${PORT:-7824}"
cat > app.lk <<'LK'
use "lib.lk"
route("GET", "/deep", fn() => response::text("" . p(100000)))
route("GET", "/endless", fn() => response::text("" . endless(0)))
route("GET", "/cb", fn() => response::text("" . cb_endless(0)))
route("GET", "/ok", fn() => response::text("alive"))
LK
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
    ( exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    got=""; for p in deep endless cb ok; do got="$got $(curl -s -m 60 -o /dev/null -w '%{http_code}' "localhost:$PORT/$p")"; done
    up=no; kill -0 "$pid" 2>/dev/null && up=yes
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$got" = " 200 500 500 200" ] && [ $up = yes ] && ok "[${mode:-default}] server: 100k levels answered, endless recursion a clean 500, server up" \
        || bad "[${mode:-default}] server: deep/endless/cb/ok -> [$got ] up: $up"
done

[ $fail = 0 ] && echo "PASS: deep recursion works, endless recursion is a clean error, nothing crashes" || { echo "FAIL: call depth"; exit 1; }

#!/usr/bin/env bash
# Call depth: deep recursion works as far as Node, endless recursion is a clean error, nothing crashes.
#
# Until fec55ef every call chain stopped at 500 (Node stops at about 9k plain / 2k through
# callbacks). A LOOK call uses heap, not the C++ stack (~330 bytes per level). The limit is 10,000
# nested calls and an 8 MB call stack per request: fec55ef's 1M calls / 256 MB added up across
# concurrent requests (12 at once reached 2.3 GB; tests/concurrent_recursion_memory_test.sh). A call made from inside a
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
# 40 locals: ~1.3 KB of registers per level, so the 8 MB budget ends it before 10,000 levels.
function fat($n) { $a1=1;$a2=2;$a3=3;$a4=4;$a5=5;$a6=6;$a7=7;$a8=8;$a9=9;$b1=1;$b2=2;$b3=3;$b4=4;$b5=5;$b6=6;$b7=7;$b8=8;$b9=9
  $c1=1;$c2=2;$c3=3;$c4=4;$c5=5;$c6=6;$c7=7;$c8=8;$c9=9;$d1=1;$d2=2;$d3=3;$d4=4;$d5=5;$d6=6;$d7=7;$d8=8;$d9=9;$e1=1;$e2=2;$e3=3;$e4=4
  if ($n <= 0) { return 0 }
  return 1 + fat($n - 1) + $a1+$a2+$a3+$a4+$a5+$a6+$a7+$a8+$a9+$b1+$b2+$b3+$b4+$b5+$b6+$b7+$b8+$b9+$c1+$c2+$c3+$c4+$c5+$c6+$c7+$c8+$c9+$d1+$d2+$d3+$d4+$d5+$d6+$d7+$d8+$d9+$e1+$e2+$e3+$e4 - 190 }
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
run "plain recursion 9,990 levels (the limit counts the whole call stack)" 'p(9990)' "ok:9990"
run "plain recursion 10,001: clean error" 'p(10001)'    "Stack overflow: more than 10000 nested calls"
run "endless recursion: clean error"     'endless(0)'   "Stack overflow: more than 10000 nested calls"
run "call stack over 8 MB: clean error"  'fat(9000)'    "Stack overflow: the call stack is larger than 8 MB"
run "callbacks 3,999 levels"             'cb(3999)'     "ok:3999"
run "callbacks 4,001 levels: clean error" 'cb(4001)'    "Stack overflow: more than 4000 nested callbacks"
run "endless callbacks: clean error"     'cb_endless(0)' "Stack overflow: more than 4000 nested callbacks"

# Small stacks: the real-stack check answers before the stack is full; ordinary callbacks still work.
# Its own small program: only the callback functions (lib.lk's long fat() expression makes the
# compiler itself recurse, which under ASan alone does not fit a 128 KB stack — not what this checks).
cat > small.lk <<'LK'
use array
function cb($n) { if ($n <= 0) { return 0 } return array::map([1], fn($x) => cb($n - 1))[0] + 1 }
function cb_endless($n) { return array::map([1], fn($x) => cb_endless($n + 1))[0] }
function try_it($f) { try { return "ok:" . $f() } catch ($e) { return "" . $e } }
print(try_it(fn() => cb_endless(0)))
print(array::map([1, 2, 3], fn($x) => $x * 2)[2])
print(try_it(fn() => cb(100)))
LK
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
route("GET", "/deep", fn() => response::text("" . p(9000)))
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
    [ "$got" = " 200 500 500 200" ] && [ $up = yes ] && ok "[${mode:-default}] server: 9,000 levels answered, endless recursion a clean 500, server up" \
        || bad "[${mode:-default}] server: deep/endless/cb/ok -> [$got ] up: $up"
done

[ $fail = 0 ] && echo "PASS: deep recursion works, endless recursion is a clean error, nothing crashes" || { echo "FAIL: call depth"; exit 1; }

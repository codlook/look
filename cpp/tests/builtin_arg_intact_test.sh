#!/usr/bin/env bash
# Passing a local variable to a built-in, or printing it, never changes the variable. For every
# built-in that runs without a server (and print/write as statements) a program passes a local
# variable — a string, an int, a list, a map — and reads it back after the call, at the top level,
# in a function and in a route. `print($a)` used to set $a to null in a function or a route
# (the VM wrote print's null result into the variable's register).
# Usage: builtin_arg_intact_test.sh <lk> <lk-fcgi>   (needs python3, curl)
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"
command -v python3 >/dev/null && command -v curl >/dev/null || { echo "  (skipped: python3 and curl needed)"; exit 0; }
T=$(mktemp -d); pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
python3 -I "$HERE/builtin_arg_intact_gen.py" "$HERE/../src/builtins.cpp" "$T" || exit 1
fail=0
want=$(wc -l < "$T/routes.txt"); want=$(( (want - 1) * 8 + 12 ))
for ctx in fn top; do
  (cd "$T" && timeout 300 "$LK" $ctx.lk 2>/dev/null) | grep '|' > "$T/$ctx.out"
  n=$(wc -l < "$T/$ctx.out"); c=$(grep -c CHANGED "$T/$ctx.out")
  if [ "$n" = "$want" ] && [ "$c" = 0 ]; then echo "  OK   $ctx: $n checks, none changed"
  else echo "  FAIL $ctx: $n of $want checks, $c changed"; grep CHANGED "$T/$ctx.out" | head -5; fail=1; fi
done
PORT=7795
(cd "$T" && LOOK_SECRET=builtin-arg-intact-0123456789abcdef0123 exec "$FCGI" --mode http --port $PORT route.lk >"$T/srv.log" 2>&1) & pid=$!
for i in $(seq 1 100); do curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/tp" && break; sleep 0.1; done
: > "$T/route.out"
while read -r r; do curl -s -m 10 "http://127.0.0.1:$PORT/$r" | grep '|' >> "$T/route.out"; done < "$T/routes.txt"
n=$(wc -l < "$T/route.out"); c=$(grep -c CHANGED "$T/route.out")
if [ "$n" = "$want" ] && [ "$c" = 0 ]; then echo "  OK   route: $n checks, none changed"
else echo "  FAIL route: $n of $want checks, $c changed"; grep CHANGED "$T/route.out" | head -5; tail -3 "$T/srv.log"; fail=1; fi

[ $fail = 0 ] && echo "PASS: a variable passed to a built-in or printed keeps its value" || { echo "FAIL: builtin argument intact"; exit 1; }

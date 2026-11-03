#!/usr/bin/env bash
# A package module still works after a hot reload.
#
# The compiler remembered the user/package modules it had loaded (~/.look/modules/<name>) for the
# life of the process, so that a module used twice is compiled once. After a hot reload — editing
# the app file on a running server — `use <name>` therefore compiled to nothing: the package's
# functions did not exist and every request that used them got 500 until the service was
# restarted. The list is now per compilation.
# Usage: package_hot_reload_test.sh <lk-fcgi>   (needs curl)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7711
T=$(mktemp -d); pid=""
trap '{ kill $pid; wait $pid; } 2>/dev/null; rm -rf "$T"' EXIT
fail=0
is() { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }

mkdir -p "$T/home/.look/modules/greet"
printf 'function greet($n) { return "hello " . $n }\n' > "$T/home/.look/modules/greet/greet.lk"
printf 'use greet\nroute("GET", "/", fn() => response::text("v1 " . greet("a")))\n' > "$T/app.lk"
( cd "$T" && exec env -i PATH=/usr/bin:/bin HOME="$T/home" LOOK_SECRET="$LOOK_SECRET" "$LKF" --mode http --port $P --workers 2 app.lk >"$T/log" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$P/" && break; sleep 0.2; done
is "the package works at start" "$(curl -s --max-time 2 "http://127.0.0.1:$P/")" "v1 hello a"

printf 'use greet\nroute("GET", "/", fn() => response::text("v2 " . greet("b")))\n' > "$T/app.lk"
touch -d '+2 seconds' "$T/app.lk"
for i in $(seq 1 30); do r=$(curl -s --max-time 1 "http://127.0.0.1:$P/"); [ "${r%% *}" = v2 ] && break
    grep -q "Hot reload" "$T/log" && [ -n "$(curl -s -o /dev/null -w '%{http_code}' --max-time 1 "http://127.0.0.1:$P/" | grep 500)" ] && break; sleep 0.3; done
is "after a hot reload the package still works" "$(curl -s --max-time 2 "http://127.0.0.1:$P/")" "v2 hello b"
grep -q "Undefined variable: greet" "$T/log" && { echo "  FAIL the log says the package function is gone"; fail=1; } || echo "  OK   no 'Undefined variable' in the log"

[ $fail = 0 ] && echo "PASS: package modules survive a hot reload" || echo "FAIL: package modules after a hot reload"
exit $fail

#!/usr/bin/env bash
# type::of() says "function" for a function on the VM — a closure, a named function, an arrow
# function — on the command line and in a web route. It said "unknown": type::is_function had been
# fixed for VM functions, type::of next to it had not, so `type::of($x) == "function"` was false.
# Usage: type_of_function_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7757}"
fail=0
cat > "$TMP/t.lk" <<'LK'
use type
function named() { return 1 }
$closure = function() { return 2 }
$arrow = fn($x) => $x
$r = type::of($closure) . "," . type::of(named) . "," . type::of($arrow) . "," . type::of(1) . "," . type::of(null)
print($r)
LK
want="function,function,function,int,null"
out="$(cd "$TMP" && "$LK" t.lk 2>&1 | tr -d '\r')"
[ "$out" = "$want" ] && echo "  OK   command line: $out" || { echo "  FAIL command line: [$out] want [$want]"; fail=1; }

cat > "$TMP/web.lk" <<'LK'
use type
function named() { return 1 }
route("GET", "/", function() {
    $closure = function() { return 2 }
    $arrow = fn($x) => $x
    return response::text(type::of($closure) . "," . type::of(named) . "," . type::of($arrow) . "," . type::of(1) . "," . type::of(null))
})
LK
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 web.lk > "$TMP/web.log" 2>&1 ) & pid=$!
for i in $(seq 1 60); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.05; done
body="$(curl -s -m 3 "localhost:$PORT/")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
[ "$body" = "$want" ] && echo "  OK   web route: $body" || { echo "  FAIL web route: [$body] want [$want]"; fail=1; }

[ $fail = 0 ] && echo "PASS: type::of says function for a function" || { echo "FAIL: type::of"; exit 1; }

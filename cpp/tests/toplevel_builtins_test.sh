#!/usr/bin/env bash
# A top-level variable computed with a built-in function has the same value in a route on
# both engines.
#
# The top level of a web application is run twice at startup, once per engine. In the VM's
# pass the plain built-ins — strtoupper, strtolower, max, min, abs, sqrt, strlen, join,
# string, bool — were not connected and returned null, so
#     $limit = max(10, int(env("PAGE_SIZE", "20")))
# at the top level was null in every route on the default engine, and correct with
# LOOK_BYTECODE=0. Nothing reported it. (count, str, int, float and the module functions
# were connected.) The request table and the setup table now share one definition.
# Usage: toplevel_builtins_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7749}"
cat > "$TMP/app.lk" <<'LK'
use json
use string
use math
$u  = strtoupper("abc")
$lo = strtolower("ABC")
$mx = max(3, 9)
$mn = min(3, 9)
$ab = abs(-4)
$sq = sqrt(81)
$sl = strlen("hello")
$jn = join([1, 2], ",")
$st = string(42)
$bo = bool(1)
$n  = count([1, 2, 3])
$s  = str(5) . int("7") . float("1.5")
$m  = string::upper("x") . math::sqrt(16)
$limit = max(10, int(env("LOOK_TEST_PAGE_SIZE", "20")))
route("GET", "/top", fn() => response::json(["upper" => $u, "lower" => $lo, "max" => $mx, "min" => $mn, "abs" => $ab, "sqrt" => $sq,
      "strlen" => $sl, "join" => $jn, "string" => $st, "bool" => $bo, "count" => $n, "conv" => $s, "module" => $m, "limit" => $limit]))
route("GET", "/in", fn() => response::json(["upper" => strtoupper("abc"), "lower" => strtolower("ABC"), "max" => max(3, 9), "min" => min(3, 9),
      "abs" => abs(-4), "sqrt" => sqrt(81), "strlen" => strlen("hello"), "join" => join([1, 2], ","), "string" => string(42), "bool" => bool(1),
      "count" => count([1, 2, 3]), "conv" => str(5) . int("7") . float("1.5"), "module" => string::upper("x") . math::sqrt(16),
      "limit" => max(10, int(env("LOOK_TEST_PAGE_SIZE", "20")))]))
LK
WANT='{"upper":"ABC","lower":"abc","max":9,"min":3,"abs":4,"sqrt":9,"strlen":5,"join":"1,2","string":"42","bool":true,"count":3,"conv":"571.5","module":"X4","limit":20}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/in" && break; sleep 0.1; done
    top="$(curl -s -m 3 "localhost:$PORT/top")"; in="$(curl -s -m 3 "localhost:$PORT/in")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$top" = "$WANT" ] && [ "$in" = "$WANT" ]; then echo "  OK   [$mode] 14 values: the same computed at the top level and inside a route"
    else echo "  FAIL [$mode] top level: $top"; echo "       [$mode] in route:  $in"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: built-ins give the same value at the top level on both engines" || { echo "FAIL: top-level built-ins"; exit 1; }

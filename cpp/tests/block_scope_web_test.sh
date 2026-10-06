#!/usr/bin/env bash
# Block scope on the WEB path. block_scope_test.lk covers the command line; a web application's
# top-level code is its setup, compiled and run once, and its variables reach the routes as
# globals — a different road through the engine, where the two engines have disagreed before.
# The same rule must hold there: a variable first assigned in a top-level block is not a
# global, so no route sees it; a variable declared before the block and assigned in it is.
# Usage: block_scope_web_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7716}"
cat > "$TMP/app.lk" <<'LK'
use json
function seen($f) { $r = "?"
    try { $r = "" . $f() } catch ($e) { $r = "undefined" }
    return $r }
$declared = "before"
$level = "quiet"
if (true) { $in_if = 1; $declared = "set in block"; $level = "verbose" }
for ($i = 0; $i < 2; $i = $i + 1) { $in_for = $i }
foreach (["a", "b"] as $item) { $in_foreach = $item }
try { $in_try = 1 } catch ($e) { $in_catch = 1 }
$names = []
foreach (["x", "y"] as $n) { push($names, $n)
    route("GET", "/item/" . $n, fn() => response::json(["item" => $n])) }
route("GET", "/scope", fn() => response::json([
    "if" => seen(fn() => $in_if), "for" => seen(fn() => $in_for), "for_init" => seen(fn() => $i),
    "foreach" => seen(fn() => $in_foreach), "foreach_var" => seen(fn() => $item),
    "try" => seen(fn() => $in_try), "declared" => $declared, "level" => $level, "names" => $names]))
LK
WANT='{"if":"undefined","for":"undefined","for_init":"undefined","foreach":"undefined","foreach_var":"undefined","try":"undefined","declared":"set in block","level":"verbose","names":["x","y"]}'
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/scope" && break; sleep 0.1; done
    got="$(curl -s -m 3 "localhost:$PORT/scope")"
    items="$(curl -s -m 3 "localhost:$PORT/item/x")$(curl -s -m 3 "localhost:$PORT/item/y")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = "$WANT" ]; then echo "  OK   [$mode] routes see only what the top level declared"
    else echo "  FAIL [$mode] got: $got $(grep -m1 -i 'error' "$TMP/log.txt" | cut -c1-160)"; fail=1; fi
    # A route registered in a top-level loop keeps the value of its own iteration.
    if [ "$items" = '{"item":"x"}{"item":"y"}' ]; then echo "  OK   [$mode] a route created in a loop keeps its own iteration's value"
    else echo "  FAIL [$mode] loop routes: $items"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: block scope holds on the web path" || { echo "FAIL: block scope (web)"; exit 1; }

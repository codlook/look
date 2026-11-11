#!/usr/bin/env bash
# Operations that walk a deeply nested value end in a clean error, never a crash.
#
# String conversion (print, ".", string::join/format, log::*), == / != (also array::unique and
# array::contains), and the copies made for cache:: and for threads walked arrays/structs nested
# inside each other with recursive C++ and no limit. Measured on the old binary: about 40-80k
# levels (3-7k under ASan) overflowed the C++ stack and the whole process died — one request took
# down the server. Now (Python's model; PHP json 512, .NET 64) past 1000 levels they raise a
# catchable "Value nested too deeply" error; up to 1000 levels everything works as before.
# array::flatten needs no limit: it is a loop now and flattens any depth.
# Usage: value_nesting_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
cd "$TMP"
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

HEAD='use array
use string
use cache
use log
use json
function deep($n) { $a = 1; for ($i = 0; $i < $n; $i = $i + 1) { $a = [$a] } return $a }
function show($v) { print($v) }
function try_it($f) { try { $f(); return "no-error" } catch ($e) { return "" . $e } }'
NE="Value nested too deeply: more than 1000 levels"

# case: label | LOOK body | expected substring of the output
run() {
    printf '%s\n%s\n' "$HEAD" "$2" > t.lk
    out=$(timeout 120 "$LK" t.lk 2>&1); rc=$?
    if [ $rc -ge 128 ]; then bad "$1: process died (exit $rc)"; return; fi
    case "$out" in *"$3"*) ok "$1";; *) bad "$1: got [$(echo "$out" | tail -c 200 | tr '\n' ' ')], want [$3]";; esac
}

D='$a = deep(1000000); $b = deep(1000000);'
run "string conversion (.)"   "$D print(try_it(fn() => \"\" . \$a))"                   "$NE"
run "print"                   "$D print(try_it(fn() => show(\$a)))"                     "$NE"
run "string::join"            "$D print(try_it(fn() => string::join(\$a, \",\")))"      "$NE"
run "string::format"          "$D print(try_it(fn() => string::format(\"%s\", \$a)))"   "$NE"
run "log::info"               "$D print(try_it(fn() => log::info(\$a)))"                "$NE"
run "=="                      "$D print(try_it(fn() => \$a == \$b))"                    "$NE"
run "!="                      "$D print(try_it(fn() => \$a != \$b))"                    "$NE"
run "array::unique"           "$D print(try_it(fn() => array::unique([\$a, \$b])))"     "$NE"
run "array::contains"         "$D print(try_it(fn() => array::contains([\$a], \$b)))"   "$NE"
run "cache::set (copy)"       "$D print(try_it(fn() => cache::set(\"k\", \$a)))"        "$NE"
run "struct chain to string"  'struct N { next any }
$n = null
for ($i = 0; $i < 100000; $i = $i + 1) { $n = N{next: $n} }
print(try_it(fn() => "" . $n))' "$NE"
run "the error is catchable and the program goes on" "$D \$m = try_it(fn() => \"\" . \$a); print(\"after \" . (\$a == \$a))" "after true"
run "worst case: inside 240 nested callbacks" 'function nest($k) {
  if ($k <= 0) { $a = deep(1000000); return try_it(fn() => "" . $a) }
  return array::map([1], fn($x) => nest($k - 1))[0]
}
print(nest(240))' "$NE"

# The limit itself: 1000 levels work everywhere (as before), 1001 is the error.
run "1000 levels work" '$a = deep(1000); $b = deep(1000); cache::set("k", $a); print(strlen("" . $a) . " " . ($a == $b) . " " . (cache::get("k") == $a))' "2001 true true"
run "1001 levels error" '$a = deep(1001); print(try_it(fn() => "" . $a))' "$NE"
run "shallow values unchanged" 'print("" . [1, [2, "x"], ["k" => 3]] . " " . ([1, [2]] == [1, [2]]) . " " . ([1, [2]] == [1, [3]]))' '[1, [2, "x"], ["k" => 3]] true false'

# json::encode: same limit, and nothing below it is cut. Before, levels past 256 were silently
# written as null — the client received damaged data with no error.
run "json::encode 1M levels"          "$D print(try_it(fn() => json::encode(\$a)))"     "$NE"
run "json::encode 300 levels is whole" '$s = json::encode(deep(300)); print((string::contains($s, "null") ? "cut " : "whole ") . strlen($s))' "whole 601"
run "json::encode 1000 levels works"  'print(strlen(json::encode(deep(1000))))'           "2001"
run "json::encode 1001 levels error"  'print(try_it(fn() => json::encode(deep(1001))))'   "$NE"
run "json::encode shallow unchanged"  'struct P { name string, n int }
print(json::encode(["a" => [1, 2.5, "x", null, true], "p" => P{name: "Ada", n: 3}]))' '{"a":[1,2.5,"x",null,true],"p":{"name":"Ada","n":3}}'

# array::flatten: no limit, same order and depth argument as before.
run "flatten 1M levels" '$a = [1]; for ($i = 0; $i < 1000000; $i = $i + 1) { $a = [$a, 2] } $f = array::flatten($a); print(count($f) . " " . $f[0] . " " . $f[1])' "1000001 1 2"
run "flatten order and depth" 'print(json::encode(array::flatten([1, [2, [3, [4]]], 5])) . " " . json::encode(array::flatten([1, [2, [3, [4]]], 5], 1)) . " " . json::encode(array::flatten([1, [2, [3]]], 0)))' '[1,2,3,4,5] [1,2,[3,[4]],5] [1,[2,[3]]]'
run "flatten keeps assoc arrays whole" 'print(json::encode(array::flatten([1, ["k" => [2]], [3]])))' '[1,{"k":[2]},3]'

# Server: one request with a deep value is a 500 and the server stays up (fibers and worker pool).
PORT="${PORT:-7823}"
cat > app.lk <<'LK'
function deep($n) { $a = 1; for ($i = 0; $i < $n; $i = $i + 1) { $a = [$a] } return $a }
route("GET", "/show", function() { return response::text("" . deep(1000000)) })
route("GET", "/eq", function() { return response::text("" . (deep(1000000) == deep(1000000))) })
route("GET", "/json", function() { return response::json(["x" => deep(2000)]) })
route("GET", "/ok", fn() => response::text("alive"))
LK
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
    ( exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > log.txt 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    got=""; for p in show eq json ok; do got="$got $(curl -s -m 60 -o /dev/null -w '%{http_code}' "localhost:$PORT/$p")"; done
    up=no; kill -0 "$pid" 2>/dev/null && up=yes
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ "$got" = " 500 500 500 200" ] && [ $up = yes ] && ok "[${mode:-default}] server: deep value is a clean 500, server up" \
        || bad "[${mode:-default}] server: show/eq/json/ok -> [$got ] up: $up"
done

[ $fail = 0 ] && echo "PASS: walking a deeply nested value ends in a clean error, never a crash" || { echo "FAIL: value nesting"; exit 1; }

#!/usr/bin/env bash
# type::of and the type::is_* functions agree, for a value of every kind — so that when one of them
# is fixed and its neighbour is forgotten (type::is_function was fixed for VM functions, type::of
# was not, and said "unknown"), this goes red.
# For each value: type::of names a known kind; the is_* of that kind is true and every other is_*
# is false (a struct has no is_* of its own: all are false). On the command line and in a web route.
# Usage: type_of_is_consistency_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7758}"
fail=0
# The check, as a LOOK function that returns "ok" or the mismatches.
cat > "$TMP/check.lk" <<'LK'
use type
use array
struct TypePoint { x int = 0 }
function named_fn() { return 1 }
function check_types() {
    $values = [1, 1.5, "s", true, [1, 2], ["k" => "v"], TypePoint { x: 1 }, null, named_fn,
               function() { return 2 }, fn($x) => $x]
    $labels = ["int", "float", "string", "bool", "list", "map", "struct", "null", "named function",
               "closure", "arrow function"]
    $bad = ""
    $i = 0
    foreach ($values as $v) {
        $of = type::of($v)
        $is = ["null" => type::is_null($v), "int" => type::is_int($v), "float" => type::is_float($v),
               "string" => type::is_string($v), "bool" => type::is_bool($v),
               "array" => type::is_array($v), "function" => type::is_function($v)]
        $known = $of == "struct" || array::contains(array::keys($is), $of)
        $true_count = 0
        $match = false
        foreach ($is as $kind => $yes) {
            if ($yes) { $true_count = $true_count + 1; if ($kind == $of) { $match = true } }
        }
        $label = $labels[$i]
        $ok = $known && (($of == "struct" && $true_count == 0) || ($match && $true_count == 1))
        if (!$ok) { $bad = $bad . "[" . $label . ": of=" . $of . " true_is=" . $true_count . "]" }
        $i = $i + 1
    }
    return $bad == "" ? "ok" : $bad
}
LK
{ cat "$TMP/check.lk"; echo 'print(check_types())'; } > "$TMP/cli.lk"
out="$(cd "$TMP" && "$LK" cli.lk 2>&1 | tr -d '\r')"
[ "$out" = ok ] && echo "  OK   command line: type::of and type::is_* agree for 11 kinds of value" \
                || { echo "  FAIL command line: $out"; fail=1; }

{ cat "$TMP/check.lk"; echo 'route("GET", "/", fn() => response::text(check_types()))'; } > "$TMP/web.lk"
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 web.lk > "$TMP/web.log" 2>&1 ) & pid=$!
for i in $(seq 1 60); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.05; done
body="$(curl -s -m 3 "localhost:$PORT/")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
[ "$body" = ok ] && echo "  OK   web route: type::of and type::is_* agree" \
                 || { echo "  FAIL web route: [$body] log=[$(grep -i error "$TMP/web.log" | head -1)]"; fail=1; }

[ $fail = 0 ] && echo "PASS: type::of and type::is_* agree" || { echo "FAIL: type::of / type::is_* consistency"; exit 1; }

#!/usr/bin/env bash
# Every pure built-in gives the same value when it is called at the top level of a web
# application and read in a route, as when it is called inside the route — on both engines.
#
# The cases are GENERATED from the binary's own built-in table (`lk --builtins`), so a
# function added later is covered without anyone remembering to add it:
#   1. for each function of a pure module (array, crypto, html, json, math, string, type,
#      validator) and each plain built-in, a list of argument shapes is tried on the tree-walk
#      command line until one runs without an error and gives the same output twice
#      (functions that are random or depend on the clock drop out here);
#   2. one web application is generated: `$v<i> = <call>` at the top level for every sampled
#      call, a route that returns those variables, and a route that makes the same calls;
#   3. it is started on the VM and on the tree-walk engine; all four lists must be equal.
# This is the class of the 1.0.13 fix: ten plain built-ins were not connected in the VM's
# setup pass and returned null at the top level, with no error.
# The number of sampled functions is printed; it must not fall below MIN_SAMPLED.
# Usage: builtin_toplevel_auto_test.sh <lk> <lk-fcgi>
LK="$(cd "$(dirname "${1:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$1")"
FCGI="$(cd "$(dirname "${2:?usage: $0 <lk> <lk-fcgi>}")" && pwd)/$(basename "$2")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7751}"; MIN_SAMPLED="${MIN_SAMPLED:-90}"
PURE_MODULES="array crypto html json math string type validator"
PLAIN="bool string strlen abs max min sqrt strtoupper strtolower join count str int float"
cat > "$TMP/shapes.txt" <<'SH'
"abc"
"Hello World"
5
-4
2.5
[3, 1, 2]
["k" => 1, "z" => 2]
"a,b,c", ","
[1, 2, 3], ","
"hello world", "o"
"hello", 1, 3
"hello", 1
3, 9
9, 2
[1, 2, 3], fn($x) => $x * 2
[1, 2, 3], fn($x) => $x > 1
[1, 2, 3], fn($a, $b) => $a + $b, 0
[1, 2, 3], 2
[1, 2, 3], [4, 5]
["k" => 1, "z" => 2], "k"
"abc", "b", "X"
"abc", 5, "-"
"{\"a\":1}"
"aGVsbG8="
[[1, 2], [3, 4]]
"5"
"user@example.com"
true
null
"abc", "secret"
"a b", " "
1, 5
SH
"$LK" --builtins > "$TMP/names.txt" || { echo "FAIL: lk --builtins"; exit 1; }
: > "$TMP/calls.txt"
while IFS= read -r name; do
    case "$name" in *rand*|*shuffle*|*uuid*|*token*|*now*|*time*) continue;; esac   # not a fixed value by design
    mod="${name%%::*}"; uses=""
    if [ "$mod" != "$name" ]; then
        case " $PURE_MODULES " in *" $mod "*) ;; *) continue;; esac
        uses="use $mod"
    else
        case " $PLAIN " in *" $name "*) ;; *) continue;; esac
    fi
    while IFS= read -r shape; do
        printf 'use json\n%s\nprint(json::encode(%s(%s)))\n' "$uses" "$name" "$shape" > "$TMP/p.lk"
        a="$(LOOK_CLI_VM=0 timeout 10 "$LK" "$TMP/p.lk" 2>/dev/null)" || continue
        b="$(LOOK_CLI_VM=0 timeout 10 "$LK" "$TMP/p.lk" 2>/dev/null)" || continue
        [ -n "$a" ] && [ "$a" = "$b" ] || continue
        case "$a" in *"<function>"*) continue;; esac   # a function printed as a value: not a meaningful sample
        echo "$name($shape)" >> "$TMP/calls.txt"; break
    done < "$TMP/shapes.txt"
done < "$TMP/names.txt"
n="$(wc -l < "$TMP/calls.txt")"
{
    echo "use json"; for m in $PURE_MODULES; do echo "use $m"; done
    i=0; while IFS= read -r c; do echo "\$v$i = $c"; i=$((i+1)); done < "$TMP/calls.txt"
    printf 'route("GET", "/top", function() { return response::text(json::encode(['
    i=0; while IFS= read -r c; do [ $i -gt 0 ] && printf ', '; printf '$v%d' $i; i=$((i+1)); done < "$TMP/calls.txt"
    echo '])) })'
    printf 'route("GET", "/in", function() { return response::text(json::encode(['
    i=0; while IFS= read -r c; do [ $i -gt 0 ] && printf ', '; printf '%s' "$c"; i=$((i+1)); done < "$TMP/calls.txt"
    echo '])) })'
} > "$TMP/app.lk"
fail=0; ref=""
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/in" && break; sleep 0.1; done
    top="$(curl -s -m 10 "localhost:$PORT/top")"; in="$(curl -s -m 10 "localhost:$PORT/in")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    [ -z "$ref" ] && ref="$in"
    if [ -n "$top" ] && [ "$top" = "$in" ] && [ "$in" = "$ref" ]; then echo "  OK   [$mode] $n functions: top level == inside a route"
    else
        echo "  FAIL [$mode] the lists differ $(grep -m1 -o 'rror.*' "$TMP/log.txt" | cut -c1-120)"; fail=1
        echo "$top" > "$TMP/a.json"; echo "$in" > "$TMP/b.json"; echo "$ref" > "$TMP/c.json"
        python3 - "$TMP" <<'PY'
import json, sys
d = sys.argv[1]
calls = [l.rstrip("\n") for l in open(d + "/calls.txt")]
def load(f):
    try: return json.load(open(d + "/" + f, encoding="utf-8", errors="replace"))
    except Exception: return None
top, inr, ref = load("a.json"), load("b.json"), load("c.json")
for nm, lst in (("top level", top), ("in route", inr)):
    if lst is None: print("       %s: no valid answer" % nm); continue
    for i, c in enumerate(calls):
        if ref is not None and i < len(lst) and i < len(ref) and lst[i] != ref[i]:
            print("       %s: %s -> %s (expected %s)" % (nm, c, json.dumps(lst[i]), json.dumps(ref[i])))
PY
    fi
done
if [ "$n" -lt "$MIN_SAMPLED" ]; then echo "  FAIL only $n functions could be sampled (want at least $MIN_SAMPLED)"; fail=1; fi
[ $fail = 0 ] && echo "PASS: $n built-ins give the same value at the top level and in a route, on both engines" || { echo "FAIL: built-ins at the top level (generated)"; exit 1; }

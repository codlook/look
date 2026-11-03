#!/usr/bin/env bash
# A double quote inside {…} in a string: the error says why and shows the right spelling.
#
# "Hi {$u["name"]}" is how many languages write it, but in LOOK the inner " ends the string and
# the rest of the line became a parse error that named something else ("Expect ')' after print
# arguments. (got 'name')"). Now the error names the placeholder and gives the corrected text
# with single quotes ({$u['name']}), which already works. A plain "{" in a string, an unclosed
# "{$" and every valid placeholder behave as before.
# Usage: interpolation_quote_message_test.sh <lk>
set -u
LK="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
run() { printf '%s\n' "$1" > "$T/t.lk"; (cd "$T" && "$LK" t.lk 2>&1); }
has() { if [[ "$2" == *"$3"* ]]; then echo "  OK   $1"; else echo "  FAIL $1: $2"; fail=1; fi; }
is()  { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }

o=$(run '$u = ["name" => "Ali"]
print("Hi {$u["name"]}")')
has 'print("Hi {$u["name"]}"): names the placeholder'  "$o" 'the " in {$u["name"]} ends the string'
has '… and gives the single-quoted spelling'           "$o" "write single quotes: {\$u['name']}"
has '… on the right line'                              "$o" "Line 2:"
o=$(run '$m = ["a" => ["b" => 1]]
$s = "x {$m["a"]["b"]} y"
print($s)')
has 'nested keys: {$m['"'"'a'"'"']['"'"'b'"'"']}'          "$o" "{\$m['a']['b']}"
o=$(run 'use cache
print("v={cache::get("k")}")')
has '{cache::get("k")}: says only {$…} is replaced'    "$o" 'Only {$…} is replaced inside a string'
is "the single-quoted spelling works"  "$(run '$u = ["name" => "Ali"]
print("Hi {$u['"'"'name'"'"']}")')" "Hi Ali"
is 'a plain { in a string'              "$(run 'print("a { b")')" "a { b"
is 'an unclosed {$ at the end'          "$(run 'print("x {$")')" 'x {$'
is '"{" . "x"'                          "$(run 'print("{" . "x")')" "{x"
is 'a valid placeholder'                "$(run '$a = [1]
print("n={$a[0]} ok")')" "n=1 ok"
is 'an escaped quote inside {…} text'   "$(run 'print("{\"a\": 1}")')" '{"a": 1}'

[ $fail = 0 ] && echo "PASS: a quote inside {…} gets an error that shows the right spelling" || echo "FAIL: quote inside {…}"
exit $fail

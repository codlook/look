#!/usr/bin/env bash
# `$l = array::push($l, $x)` and `$l = array::slice($l, 0, -1)` compile to an in-place step that skips
# the module check, so they ran without `use array` while `$m["k"] = array::push(...)` stopped with
# "Module 'array' not loaded." — a hidden rule. Same schedule as push/pop: this release warns, a later
# one makes it an error.
#   * `lk --check` : one WARN [use-array-missing] line per file (first place + total); exit code unchanged
#   * at run time  : one [WARN ] [check] line per place (not per loop turn); the program still runs
# A file with `use array` is not reported; the path form still stops with the module error.
# Usage: use_array_missing_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

cat > "$TMP/nouse.lk" <<'LK'
$l = [1, 2, 3]
for ($i = 0; $i < 5; $i = $i + 1) { $l = array::push($l, $i) }
$l = array::slice($l, 0, -1)
print(json::encode($l))
LK
out="$("$LK" --check "$TMP/nouse.lk" 2>&1)"; rc=$?
n="$(printf '%s\n' "$out" | grep -c '\[use-array-missing\]')"
if [ "$rc" = 0 ] && [ "$n" = 1 ] && printf '%s\n' "$out" | grep -q '^WARN 2 .*\[use-array-missing\] array:: is used without `use array` (2 places in this file, the first here)'; then
  ok "--check: one WARN [use-array-missing] with the first place and the total, exit 0"
else bad "--check: rc=$rc out=[$out]"; fi

o="$("$LK" "$TMP/nouse.lk" 2>"$TMP/err.txt")"; rc=$?
nw="$(grep -c 'is used without `use array`' "$TMP/err.txt")"
if [ "$rc" = 0 ] && [ "$o" = "[1,2,3,0,1,2,3]" ] && [ "$nw" = 2 ] \
   && grep -q 'line 2: array::push() is used without `use array`' "$TMP/err.txt" \
   && grep -q 'line 3: array::slice() is used without `use array`' "$TMP/err.txt"; then
  ok "run: still works, one warning per place (not per loop turn)"
else bad "run: rc=$rc out=[$o] warnings=$nw stderr=[$(cat "$TMP/err.txt")]"; fi

printf 'use array\n' | cat - "$TMP/nouse.lk" > "$TMP/use.lk"
out="$("$LK" --check "$TMP/use.lk" 2>&1)"; o="$("$LK" "$TMP/use.lk" 2>"$TMP/err2.txt")"
if [ "$out" = "OK" ] && [ "$o" = "[1,2,3,0,1,2,3]" ] && ! grep -q 'use array' "$TMP/err2.txt"; then
  ok "with use array: no warning"
else bad "with use array: check=[$out] out=[$o] stderr=[$(cat "$TMP/err2.txt")]"; fi

printf '$m = ["a" => [1]]\n$m["a"] = array::push($m["a"], 2)\n' > "$TMP/path.lk"
e="$("$LK" "$TMP/path.lk" 2>&1)"
case "$e" in *"Module 'array' not loaded."*) ok "the path form still stops with the module error";;
  *) bad "path form: [$e]";; esac

[ $fail = 0 ] && echo "PASS: in-place array:: without use array is reported" || { echo "FAIL: use array missing"; exit 1; }

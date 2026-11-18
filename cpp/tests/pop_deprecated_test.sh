#!/usr/bin/env bash
# Bare pop($list) and array::pop($list) are deprecated (same schedule as push): pop() changes the
# variable it is given, and array::pop() returns the last element WITHOUT removing it (the name says
# otherwise). The one way: $x = $list[-1] and $list = array::slice($list, 0, -1). During the
# deprecation release both still work and each is reported once per file:
#   * `lk --check` : one WARN line per kind ([pop-deprecated], [array-pop-deprecated]) with the first
#                    call's place and the total; exit code unchanged
#   * at load      : one [WARN ] [check] line per kind, the program still runs
# A file that uses only $l[-1] and array::slice is not reported.
# Usage: pop_deprecated_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
POP='pop($list) changes the variable it is given'
APOP='array::pop($list) does not remove anything, it returns the last element'

cat > "$TMP/bare.lk" <<'LK'
use array
$l = [1, 2, 3, 4, 5]
pop($l)
$x = pop($l)
pop($l)
$a = array::pop($l)
$b = array::pop([7, 8])
print(json::encode([$l, $x, $a, $b]))
LK
cat > "$TMP/new.lk" <<'LK'
use array
$l = [1, 2, 3, 4, 5]
$x = $l[-1]
$l = array::slice($l, 0, -1)
print(json::encode([$l, $x]))
LK

# lk --check: her tür için tam bir uyarı, ilk çağrının satırı + toplam; çıkış 0, son satır OK
out="$("$LK" --check "$TMP/bare.lk" 2>&1)"; rc=$?
n1="$(grep -c '^WARN .*\[pop-deprecated\]' <<< "$out")"
n2="$(grep -c '^WARN .*\[array-pop-deprecated\]' <<< "$out")"
if [ "$n1" = 1 ] && [ "$n2" = 1 ] && grep -q '^WARN 3 1 \[pop-deprecated\] .*(3 calls in this file' <<< "$out" \
   && grep -q '^WARN 6 [0-9]* \[array-pop-deprecated\] .*(2 calls in this file' <<< "$out" \
   && grep -qF -- "$POP" <<< "$out" && grep -qF -- "$APOP" <<< "$out" \
   && grep -qF -- 'array::slice($list, 0, -1)' <<< "$out" && grep -qF -- 'lk fix rewrites it' <<< "$out" \
   && [ "$rc" = 0 ] && [ "$(tail -1 <<< "$out")" = "OK" ]; then
  echo "  OK   --check: one [pop-deprecated] and one [array-pop-deprecated] warning, exit 0"
else echo "  FAIL --check on pop/array::pop: rc=$rc warnings=$n1/$n2 out=[$out]"; fail=1; fi

# yüklemede: tür başına tek WARN satırı, program çalışır, pop/array::pop hâlâ çalışır
o="$("$LK" "$TMP/bare.lk" 2>"$TMP/err.txt")"; rc=$?
n1="$(grep -cF -- "$POP" "$TMP/err.txt")"; n2="$(grep -cF -- "$APOP" "$TMP/err.txt")"
if [ "$o" = "[[1,2],4,2,8]" ] && [ "$rc" = 0 ] && [ "$n1" = 1 ] && [ "$n2" = 1 ] \
   && grep -q 'WARN.*\[check\] .*bare.lk:3: pop(' "$TMP/err.txt" && grep -q 'WARN.*\[check\] .*bare.lk:6: array::pop(' "$TMP/err.txt"; then
  echo "  OK   at load: one warning per kind, pop() and array::pop() still work"
else echo "  FAIL at load: output=[$o] rc=$rc warnings=$n1/$n2 stderr=[$(cat "$TMP/err.txt")]"; fail=1; fi

# yeni yazım: uyarı yok
out="$("$LK" --check "$TMP/new.lk" 2>&1)"; rc=$?
o="$("$LK" "$TMP/new.lk" 2>"$TMP/err2.txt")"; rc2=$?
if [ "$out" = "OK" ] && [ "$rc" = 0 ] && [ "$o" = "[[1,2,3,4],5]" ] && [ "$rc2" = 0 ] && ! grep -q 'deprecated\|pop(' "$TMP/err2.txt"; then
  echo "  OK   \$l[-1] + array::slice: no warning"
else echo "  FAIL new way: check=[$out] rc=$rc output=[$o] rc=$rc2 stderr=[$(cat "$TMP/err2.txt")]"; fail=1; fi

[ $fail = 0 ] && echo "PASS: pop() and array::pop() are reported once per file and still work" || { echo "FAIL: pop deprecation"; exit 1; }

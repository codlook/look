#!/usr/bin/env bash
# Bare push($list, $x) is deprecated: it changes the variable it is given, where everything else in
# LOOK returns a new value ($list = array::push($list, $x), as Go's a = append(a, x)). During the
# deprecation release push() still works and is reported once per file:
#   * `lk --check` : one WARN line [push-deprecated] (first call's place + total), exit code unchanged
#   * at load      : one [WARN ] [check] line, the program still runs
# A file that uses only array::push is not reported.
# Usage: push_deprecated_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
MSG='push($list, $x) changes the variable it is given'

cat > "$TMP/bare.lk" <<'LK'
$l = [1]
push($l, 2)
push($l, 3)
push($l, 4)
print(json::encode($l))
LK
cat > "$TMP/new.lk" <<'LK'
use array
$l = [1]
$l = array::push($l, 2)
$l = array::push($l, 3)
print(json::encode($l))
LK

# lk --check: tam bir uyarı, ilk çağrının satırı (2), toplam 3; çıkış kodu 0, son satır OK
out="$("$LK" --check "$TMP/bare.lk" 2>&1)"; rc=$?
n="$(grep -c '^WARN .*\[push-deprecated\]' <<< "$out")"
if [ "$n" = 1 ] && grep -q '^WARN 2 [0-9]* \[push-deprecated\] .*(3 calls in this file' <<< "$out" \
   && grep -qF -- "$MSG" <<< "$out" && [ "$rc" = 0 ] && [ "$(tail -1 <<< "$out")" = "OK" ]; then
  echo "  OK   --check: one [push-deprecated] warning for 3 calls, exit 0"
else echo "  FAIL --check on bare push: rc=$rc warnings=$n out=[$out]"; fail=1; fi

# yüklemede: tek WARN satırı, program çalışır, push hâlâ çalışır
o="$("$LK" "$TMP/bare.lk" 2>"$TMP/err.txt")"; rc=$?
n="$(grep -cF -- "$MSG" "$TMP/err.txt")"
if [ "$o" = "[1,2,3,4]" ] && [ "$rc" = 0 ] && [ "$n" = 1 ] && grep -q 'WARN.*\[check\] .*bare.lk:2: push(' "$TMP/err.txt"; then
  echo "  OK   at load: one warning, push() still works"
else echo "  FAIL at load: output=[$o] rc=$rc warnings=$n stderr=[$(cat "$TMP/err.txt")]"; fail=1; fi

# array::push: uyarı yok
out="$("$LK" --check "$TMP/new.lk" 2>&1)"; rc=$?
o="$("$LK" "$TMP/new.lk" 2>"$TMP/err2.txt")"; rc2=$?
if [ "$out" = "OK" ] && [ "$rc" = 0 ] && [ "$o" = "[1,2,3]" ] && [ "$rc2" = 0 ] && ! grep -qF -- "$MSG" "$TMP/err2.txt"; then
  echo "  OK   array::push: no warning"
else echo "  FAIL array::push: check=[$out] rc=$rc output=[$o] rc=$rc2 stderr=[$(cat "$TMP/err2.txt")]"; fail=1; fi

[ $fail = 0 ] && echo "PASS: bare push() is reported once per file and still works" || { echo "FAIL: push deprecation"; exit 1; }

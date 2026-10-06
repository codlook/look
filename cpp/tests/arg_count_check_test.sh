#!/usr/bin/env bash
# LOOK 2 transition check: `lk --check` lists calls to a named function of the same file with
# the wrong number of arguments. The two engines disagree on these today (the tree-walk
# engine raises an error, the VM accepts the call and a missing parameter is null), so the
# places are counted before one rule is chosen. Warnings only: exit code and "OK" unchanged.
# A parameter with a default is optional; a variadic function takes any number above its
# required ones.
# Usage: arg_count_check_test.sh <lk>
LK="${1:?usage: $0 <lk>}"; TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT; fail=0
cat > "$TMP/s.lk" <<'LK'
function one($a) { return $a }
function opt($a, $b = 2) { return $a }
function va($a, ...$r) { return $a }
one(1, 2)
one()
opt(1)
opt()
opt(1, 2, 3)
va(1, 2, 3)
va()
one(1)
$f = fn($x) => one($x, $x)
LK
out="$("$LK" --check "$TMP/s.lk" 2>&1)"; rc=$?
got="$(echo "$out" | sed -nE 's/^WARN ([0-9]+) [0-9]+ \[arg-count\] ([a-z]+)\(\).*/\1 \2/p' | tr '\n' ' ')"
[ "$got" = "4 one 5 one 7 opt 8 opt 10 va 12 one " ] || { echo "  FAIL warnings: $got"; fail=1; }
[ $rc = 0 ] && [ "$(echo "$out" | tail -1)" = "OK" ] || { echo "  FAIL exit code or last line changed (rc=$rc)"; fail=1; }
[ $fail = 0 ] && echo "PASS: lk --check reports calls with the wrong number of arguments" || { echo "FAIL: arg count check"; exit 1; }

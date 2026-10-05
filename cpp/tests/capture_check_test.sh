#!/usr/bin/env bash
# LOOK 2 transition check: `lk --check` reports, from the parser's own tree, every place the
# planned closure rule ("a closure captures the value and may not change it") would break.
# Warnings only: the exit code and the final "OK" line do not change.
#   capture-write  the closure writes to a captured variable
#   capture-stale  the variable changes after the closure captured it
#   capture-self   the closure captures the variable it is being assigned to
# The sample has one of each and two closures that must stay silent (own local with a name
# the outer function also uses; arrow function that only reads).
# Usage: capture_check_test.sh <lk>
LK="${1:?usage: $0 <lk>}"; D="$(dirname "$0")/capture_check"; fail=0
out="$("$LK" --check "$D/sample.lk" 2>&1)"; rc=$?
want="3 [capture-stale] \$g
7 [capture-write] \$n
15 [capture-stale] \$n
9 [capture-write] \$list
10 [capture-write] \$list
15 [capture-stale] \$n
20 [capture-stale] \$i
24 [capture-self] \$fib"
got="$(echo "$out" | sed -nE 's/^WARN ([0-9]+) [0-9]+ (\[[a-z-]+\]) [^$]*(\$[a-z]+).*/\1 \2 \3/p')"
[ "$got" = "$want" ] || { echo "  FAIL warnings differ:"; echo "$got"; fail=1; }
[ $rc = 0 ] && [ "$(echo "$out" | tail -1)" = "OK" ] || { echo "  FAIL exit code or last line changed (rc=$rc)"; fail=1; }
printf 'function f($rows) { $m = 3\n  $t = function() { $tmp = 1\n    return $tmp }\n  return array::map($rows, fn($r) => $r * $m) }\n$m = 1\n' > /tmp/cc_clean.lk
[ "$("$LK" --check /tmp/cc_clean.lk 2>&1)" = "OK" ] || { echo "  FAIL clean file produced warnings"; fail=1; }
[ $fail = 0 ] && echo "PASS: capture check reports writes, stale captures and self captures" || { echo "FAIL: capture check"; exit 1; }

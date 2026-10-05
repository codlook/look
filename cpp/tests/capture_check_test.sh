#!/usr/bin/env bash
# LOOK 2 closure rule, as seen at load (include/look/capture_check.h):
#   * a closure that changes a variable it captured is a PARSE ERROR, on either engine and
#     before anything runs: $x = ..., $x[i] = ..., $x.f = ..., $x += ..., $x++, push($x, ...),
#     pop($x); the same for a named function declared inside another function;
#   * a variable that changes after a closure captured it is reported as a WARNING by
#     `lk --check` (capture-stale): the closure keeps the value it captured, where LOOK 1 saw
#     the later one. The exit code and the final "OK" line do not change.
# The own local of a closure, even with a name the outer function also uses, and an arrow
# function that only reads, must stay silent.
# Usage: capture_check_test.sh <lk>
LK="${1:?usage: $0 <lk>}"; D="$(dirname "$0")/capture_check"; fail=0
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
out="$("$LK" --check "$D/sample.lk" 2>&1)"; rc=$?
want="3 [capture-stale] \$g
10 [capture-stale] \$n
15 [capture-stale] \$i"
got="$(echo "$out" | sed -nE 's/^WARN ([0-9]+) [0-9]+ (\[[a-z-]+\]) [^$]*(\$[a-z]+).*/\1 \2 \3/p')"
[ "$got" = "$want" ] || { echo "  FAIL warnings differ:"; echo "$got"; fail=1; }
[ $rc = 0 ] && [ "$(echo "$out" | tail -1)" = "OK" ] || { echo "  FAIL exit code or last line changed (rc=$rc)"; fail=1; }
cat > "$TMP/clean.lk" <<'LK'
function f($rows) { $m = 3
  $t = function() { $tmp = 1
    return $tmp }
  return array::map($rows, fn($r) => $r * $m) }
$m = 1
LK
[ "$("$LK" --check "$TMP/clean.lk" 2>&1)" = "OK" ] || { echo "  FAIL clean file produced warnings"; fail=1; }

refused() {  # label, source on stdin
    { echo 'print("ran")'; cat; } > "$TMP/w.lk"
    for env in LOOK_VM_STRICT=1 LOOK_CLI_VM=0; do
        o="$(env $env "$LK" "$TMP/w.lk" 2>&1)"
        case "$o" in
          *ran*) echo "  FAIL [$env] $1: the program ran"; fail=1;;
          *"captures the value and cannot change it"*) echo "  OK   [$env] refused at load: $1";;
          *) echo "  FAIL [$env] $1: $(echo "$o" | tr '\n' ' ' | cut -c1-160)"; fail=1;;
        esac
    done
}
refused "assign after read" <<'LK'
function f() { $n = 0
  $g = function() { $n = $n + 1
    return $n } }
LK
refused "element assignment" <<'LK'
function f() { $a = [1]
  $g = function() use ($a) { $a[0] = 2 } }
LK
refused "field assignment" <<'LK'
struct P { x int }
function f() { $p = P{}
  $g = function() { $p.x = 2 } }
LK
refused "push" <<'LK'
function f() { $a = []
  $g = function($x) { push($a, $x) } }
LK
refused "pop" <<'LK'
function f() { $a = [1]
  $g = fn() => pop($a) }
LK
refused "compound assignment" <<'LK'
function f() { $n = 0
  $g = function() { $n += 1 } }
LK
refused "increment" <<'LK'
function f() { $n = 0
  $g = function() { $n++ } }
LK
refused "top level, use list" <<'LK'
$a = [1]
$g = function() use ($a) { $a[0] = 2 }
LK
refused "nested closure" <<'LK'
function f() { $n = [0]
  $g = function() { return function() { $n[0] = 1 } } }
LK
refused "named inner function" <<'LK'
function f() { $n = [0]
  function inner() { $n[0] = 1 }
  inner() }
LK
[ $fail = 0 ] && echo "PASS: closures capture values; writes to a captured variable are refused at load" || { echo "FAIL: capture check"; exit 1; }

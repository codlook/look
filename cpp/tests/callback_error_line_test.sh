#!/usr/bin/env bash
# An error inside a callback is reported at its own line (and file), not at the line that called
# array::map / filter / sort.
#
# Each nested VM run recorded the line as the error passed through it on the way out, so the
# outermost — the builtin's call site — won: "Division by zero, Line: 6" for an error on line 4.
# The tree-walk engine of 1.0.x reported line 4; this was lost in the move to the VM. Now the
# first (innermost) record stays until a LOOK try/catch handles the error.
# Usage: callback_error_line_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP"; fail=0; n=0

# check <file> <want-line> [<want-file>]
check() {
  n=$((n + 1))
  local out; out=$("$LK" "$1" 2>&1)
  local line; line=$(grep -oE 'Line: [0-9]+' <<< "$out" | head -1)
  if [ "$line" != "Line: $2" ]; then echo "  FAIL $1: reported '$line', want 'Line: $2'"; echo "$out" | sed 's/^/       /' | head -4; fail=1; fi
  if [ -n "$3" ] && ! grep -q "File: .*$3" <<< "$out"; then echo "  FAIL $1: file not '$3': $(grep File <<< "$out")"; fail=1; fi
}

cat > named.lk <<'E'
use array
function bad($x) {
  $y = 1
  return $x / 0
}
$r = array::map([1, 2], fn($v) => bad($v))
E
check named.lk 4

cat > closure.lk <<'E'
use array
$r = array::map([1, 2], function($v) {
  $a = 1
  $b = 2
  return $v / 0
})
E
check closure.lk 5

cat > nested.lk <<'E'
use array
function inner($x) {
  return $x / 0
}
function outer($x) {
  return array::filter([$x], fn($y) => inner($y) > 0)
}
$r = array::map([1], fn($v) => outer($v))
E
check nested.lk 3

cat > lib.lk <<'E'
function from_lib($x) {
  return $x / 0
}
E
cat > uselib.lk <<'E'
use array
use "lib.lk"
$r = array::map([1], fn($v) => from_lib($v))
E
check uselib.lk 2 lib.lk

cat > caught_then_new.lk <<'E'
use array
try {
  $r = array::map([1], function($v) { return $v / 0 })
} catch ($e) {
  print("caught")
}
$z = 1
$q = $z / 0
E
check caught_then_new.lk 8

cat > sort_cmp.lk <<'E'
use array
$s = array::sort([3, 1, 2], function($a, $b) {
  return $a / 0
})
E
check sort_cmp.lk 3

[ $fail = 0 ] && echo "PASS: an error inside a callback is reported at its own line ($n cases)" \
              || { echo "FAIL: an error inside a callback was reported at the wrong line"; exit 1; }

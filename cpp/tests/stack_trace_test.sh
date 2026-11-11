#!/usr/bin/env bash
# An uncaught runtime error prints the chain of calls that led to it ("Stack trace: at f (line N)"),
# innermost first — as LOOK printed before the VM became its only engine (the tree-walk interpreter
# did; the VM printed only the error's own line). Also pins: an fn($x) => … closure carries its
# line, so an error inside one is reported at a line.
# Usage: stack_trace_test.sh <lk>
set -u
LK="${1:?usage: stack_trace_test.sh <lk>}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
# $1 label, $2 file; stdin = expected stderr (exact)
check() {
  local want got; want=$(cat); got=$(cd "$T" && "$LK" "$2" 2>&1 >/dev/null)
  if [ "$got" == "$want" ]; then echo "  OK   $1"; else echo "  FAIL $1"; diff <(echo "$want") <(echo "$got") | sed 's/^/       /'; fail=1; fi
}

cat > "$T/a.lk" <<'EOF'
function inner($x) {
  return $x / 0
}
function outer($y) {
  return inner($y) + 1
}
print(outer(5))
EOF
check "nested functions" a.lk <<'EOF'

Runtime Error: Division by zero
  File: a.lk
  Line: 2
Stack trace:
  at inner (line 5)
  at outer (line 7)
EOF

cat > "$T/b.lk" <<'EOF'
use array
function leaf($x) {
  return $x / 0
}
function mid($a) {
  return array::map($a, fn($v) => leaf($v))
}
$f = function($q) {
  return mid($q)
}
print($f([1]))
EOF
check "through a callback and closures" b.lk <<'EOF'

Runtime Error: Division by zero
  File: b.lk
  Line: 3
Stack trace:
  at leaf (line 6)
  at <closure> (line 6)
  at mid (line 9)
  at <closure> (line 11)
EOF

cat > "$T/lib.lk" <<'EOF'
function helper($x) {
  return $x + "zz"
}
function twice($x) {
  return helper($x)
}
EOF
cat > "$T/c.lk" <<'EOF'
use "lib.lk"
function go() {
  return twice(1)
}
go()
EOF
check "a call made inside a used file names that file" c.lk <<'EOF'

Runtime Error: Arithmetic on a string that cannot be converted to a number: 'zz'
  File: lib.lk
  Line: 2
Stack trace:
  at helper (line 5 in lib.lk)
  at twice (line 3)
  at go (line 5)
EOF

cat > "$T/d.lk" <<'EOF'
function f() {
  throw "boom"
}
f()
EOF
check "throw" d.lk <<'EOF'

Runtime Error: boom
  File: d.lk
  Line: 2
Stack trace:
  at f (line 4)
EOF

cat > "$T/e.lk" <<'EOF'
print(1 / 0)
EOF
check "top level: no trace" e.lk <<'EOF'

Runtime Error: Division by zero
  File: e.lk
  Line: 1
EOF

cat > "$T/f.lk" <<'EOF'
$g = fn($v) => $v / 0
$g(1)
EOF
check "an fn(…) => … closure has a line" f.lk <<'EOF'

Runtime Error: Division by zero
  File: f.lk
  Line: 1
Stack trace:
  at <closure> (line 2)
EOF

cat > "$T/g.lk" <<'EOF'
use array
function f($n) {
  return 1 / $n
}
$r = "none"
try { array::map([0], fn($v) => f($v)) } catch ($e) { $r = "caught" }
print($r)
EOF
got=$(cd "$T" && "$LK" g.lk 2>&1); rc=$?
if [ "$got" == "caught" ] && [ $rc = 0 ]; then echo "  OK   a caught error prints nothing"; else echo "  FAIL caught: rc=$rc [$got]"; fail=1; fi

cat > "$T/h.lk" <<'EOF'
function deep($n) {
  if ($n == 0) { return 1 / 0 }
  return deep($n - 1)
}
deep(500)
EOF
got=$(cd "$T" && "$LK" h.lk 2>&1)
at=$(echo "$got" | grep -c '^  at deep (line 3)$'); last=$(echo "$got" | tail -1)
if [ "$at" = 99 ] && echo "$got" | grep -qx '  \.\.\. 401 more calls \.\.\.' && [ "$last" = "  at deep (line 5)" ]; then
  echo "  OK   deep recursion: 50 innermost, a gap line, 50 outermost"
else echo "  FAIL deep recursion: at=$at last=[$last]"; echo "$got" | head -8; fail=1; fi

[ $fail = 0 ] && echo "PASS: an uncaught error prints its call chain" || { echo "FAIL: stack trace"; exit 1; }

#!/usr/bin/env bash
# print() writes its whole line at once (like Go's Println):
#  - printing a local variable leaves it unchanged — each piece used to be a separate write whose
#    null result landed in the argument's register, so `$a = 5; print($a); print($a)` printed
#    "5" then "null", and a top-level `for ($i …) { print($i) }` stopped with an arithmetic error;
#  - lines printed by parallel() tasks and the script never run into each other.
# Usage: print_line_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
# $1 label, $2 file; stdin = expected stdout (exact)
check() {
  local want got; want=$(cat); got=$(cd "$T" && timeout 20 "$LK" "$2" 2>&1)
  if [ "$got" == "$want" ]; then echo "  OK   $1"; else echo "  FAIL $1"; diff <(echo "$want") <(echo "$got") | head -8 | sed 's/^/       /'; fail=1; fi
}

cat > "$T/a.lk" <<'EOF'
function f() {
  $a = 5
  print($a)
  print($a)
  $s = "x"
  print($s, $a)
  return $a + 1
}
print(f())
EOF
check "a printed local keeps its value" a.lk <<'EOF'
5
5
x 5
6
EOF

cat > "$T/b.lk" <<'EOF'
for ($i = 0; $i < 3; $i++) { print($i) }
$t = 0
foreach ([1, 2] as $v) { print($v); $t = $t + $v }
print($t)
EOF
check "a top-level loop variable survives print" b.lk <<'EOF'
0
1
2
1
2
3
EOF

cat > "$T/c.lk" <<'EOF'
print("a", 1, [1, "b"], null, 1.5)
print()
write("no newline")
write(" ", 2)
print("")
EOF
check "separators, empty print, write" c.lk <<'EOF'
a 1 [1, "b"] null 1.5

no newline  2
EOF

cat > "$T/d.lk" <<'EOF'
$done = channel(8)
for ($k = 0; $k < 4; $k++) {
  parallel(function() {
    for ($j = 0; $j < 3000; $j++) { print("task", $k, "line", $j) }
    send($done, 1)
  })
}
for ($m = 0; $m < 3000; $m++) { print("main", "line", $m) }
for ($k = 0; $k < 4; $k++) { receive($done) }
EOF
for run in 1 2 3; do
  out=$(cd "$T" && timeout 60 "$LK" d.lk 2>/dev/null)
  n=$(echo "$out" | wc -l); bad=$(echo "$out" | grep -cvE '^(task [0-3] line [0-9]+|main line [0-9]+)$')
  if [ "$n" = 15000 ] && [ "$bad" = 0 ]; then echo "  OK   concurrent lines stay whole (run $run)"
  else echo "  FAIL concurrent lines (run $run): $n lines, $bad broken"; echo "$out" | grep -vE '^(task [0-3] line [0-9]+|main line [0-9]+)$' | head -3; fail=1; fi
done

[ $fail = 0 ] && echo "PASS: print writes its whole line at once" || { echo "FAIL: print line"; exit 1; }

#!/usr/bin/env bash
# args_test — args() builtin: CLI arguments after the script name reach the program,
# and are empty when none are passed.
# args() was the case that slipped past the doc guard, so it earns an explicit test.
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/a.lk" <<'LK'
$a = args()
print(count($a))
foreach ($a as $x) { print($x) }
LK

check() { # desc expected actual
  if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: expected [$2] got [$3]"; fail=1; fi
}

# two args
out=$("$LK" "$TMP/a.lk" up --force 2>/dev/null | tr '\n' ',')
check "two args reach args()" "2,up,--force," "$out"

# one arg
out=$("$LK" "$TMP/a.lk" solo 2>/dev/null | tr '\n' ',')
check "one arg reaches args()" "1,solo," "$out"

# no args → empty array
out=$("$LK" "$TMP/a.lk" 2>/dev/null | tr '\n' ',')
check "no args -> empty" "0," "$out"

# -c inline: args after the code string
out=$("$LK" -c 'print(count(args())); foreach (args() as $x){ print($x) }' a b c 2>/dev/null | tr '\n' ',')
check "-c: args after code string" "3,a,b,c," "$out"

if [ "$fail" = 0 ]; then echo "PASS: args() — CLI arguments reach the program"; else echo "FAIL: args()"; exit 1; fi

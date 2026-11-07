#!/usr/bin/env bash
# Numeric edge cases that used to be undefined behaviour in C++ or silently wrong:
#   * int vs float comparisons are exact: 9007199254740993 == 9007199254740992.0 was true (the int
#     was turned into a double); now false, and ordering is exact too;
#   * abs(INT64_MIN) was a signed overflow (UB) — now the float 9223372036854775808;
#   * int(1e19), math::floor(1e300) cast an out-of-range double to int64 (UB; int(1e19) gave
#     INT64_MIN) — now int() saturates and floor/ceil/round return a float when the result does not
#     fit, as + - * already do;
#   * -8 >> 70 is -1 (arithmetic shift), like -8 >> 63; it was 0.
# Run it also with an UBSan build: it must report nothing.
# Usage: numeric_edges_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: numeric_edges_test.sh <lk>}")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fail=0
while IFS='|' read -r expr want; do
    [ -z "$expr" ] && continue
    printf 'use math\nprint(%s)\n' "$expr" > "$T/a.lk"
    got="$("$LK" "$T/a.lk" 2>&1 | grep -v '^\[20' | tail -1)"
    if [ "$got" = "$want" ]; then :; else echo "  FAIL $expr: want '$want', got '$got'"; fail=1; fi
    echo "$got" | grep -q "runtime error" && { echo "  FAIL $expr: UBSan report"; fail=1; }
done <<'EOF'
9007199254740993 == 9007199254740992.0|false
9007199254740993 > 9007199254740992.0|true
9007199254740992 == 9007199254740992.0|true
9223372036854775807 == 9223372036854775807.0|false
9223372036854775807 < 9223372036854775807.0|true
(-9223372036854775807 - 1) == -9223372036854775808.0|true
1 == 1.0|true
2 < 2.5|true
-3 < -2.5|true
2.5 >= 2|true
abs(-9223372036854775807 - 1)|9223372036854775808
math::abs(-9223372036854775807 - 1)|9223372036854775808
abs(-5)|5
math::floor(1e300)|1e+300
math::ceil(-1e300)|-1e+300
math::floor(-2.5)|-3
math::round(2.5)|3
math::floor(9007199254740993)|9007199254740993
int(1e19)|9223372036854775807
int(-1e300)|-9223372036854775808
int(2.9)|2
-8 >> 70|-1
-8 >> 63|-1
8 >> 70|0
(-9223372036854775807 - 1) / -1|9223372036854775808
(-9223372036854775807 - 1) % -1|0
EOF
[ $fail = 0 ] && echo "PASS: numeric edge cases are defined and exact" || { echo "FAIL: numeric edge cases"; exit 1; }

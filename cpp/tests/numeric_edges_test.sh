#!/usr/bin/env bash
# LOOK's numeric rules at their edges — the way widely used languages behave, never a silently
# wrong value, never undefined behaviour in C++:
#   * integer overflow (+ - * / ** unary minus abs) is an error, like Swift / Rust debug / C#
#     checked; it used to become a float and lose precision (…810 printed as …808);
#   * an integer literal that does not fit 64 bits is a parse error (Go); -9223372036854775808 is
#     allowed; hex/binary literals are 64-bit patterns (0xFFFFFFFFFFFFFFFF == -1, as in Java);
#   * int ** non-negative int is an exact int (Python); a negative exponent gives a float;
#     0 ** negative is a division by zero;
#   * % with a float operand is the floating remainder (Go math.Mod, C fmod, JS): 7.5 % 2 == 1.5;
#   * a negative shift count is an error (Go); -8 >> 70 == -1;
#   * int() / float() of a numeric string out of range is an error (Go strconv ErrRange);
#     a non-numeric string still gives 0;
#   * int/float comparisons are exact; Infinity is IEEE, but json::encode refuses it (Go);
#   * int(1e19) saturates and floor/ceil/round(1e300) stay floats (were UB).
# Run it also with an UBSan build: it must report nothing.
# Usage: numeric_edges_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: numeric_edges_test.sh <lk>}")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fail=0; n=0
while IFS='|' read -r expr want; do
    [ -z "$expr" ] && continue
    n=$((n+1))
    printf 'use math\nuse type\nprint(%s)\n' "$expr" > "$T/a.lk"
    out="$("$LK" "$T/a.lk" 2>&1 | grep -v '^\[20')"
    if echo "$out" | grep -q "runtime error"; then echo "  FAIL $expr: UBSan report"; fail=1; continue; fi
    case "$want" in
        ERR:*) echo "$out" | grep -qF -- "${want#ERR:}" || { echo "  FAIL $expr: want an error containing '${want#ERR:}', got: $(echo "$out" | tr '\n' ' ' | cut -c1-120)"; fail=1; } ;;
        *)     got="$(echo "$out" | tail -1)"; [ "$got" = "$want" ] || { echo "  FAIL $expr: want '$want', got '$(echo "$out" | tr '\n' ' ' | cut -c1-120)'"; fail=1; } ;;
    esac
done <<'EOF'
9223372036854775807 + 1|ERR:Integer overflow: 9223372036854775807 + 1
-9223372036854775807 - 2|ERR:Integer overflow
9223372036854775807 * 2|ERR:Integer overflow
(-9223372036854775807 - 1) / -1|ERR:Integer overflow
-(-9223372036854775807 - 1)|ERR:Integer overflow
abs(-9223372036854775807 - 1)|ERR:Integer overflow
math::abs(-9223372036854775807 - 1)|ERR:Integer overflow
9223372036854775806 + 1|9223372036854775807
-9223372036854775808|-9223372036854775808
9223372036854775808|ERR:does not fit in 64 bits
18446744073709551615|ERR:does not fit in 64 bits
0xFFFFFFFFFFFFFFFF|-1
2 ** 62|4611686018427387904
3 ** 39|4052555153018976267
(-2) ** 63|-9223372036854775808
2 ** 63|ERR:Integer overflow: 2 ** 63
2 ** -1|0.5
0 ** -1|ERR:Division by zero
0.0 ** -1|ERR:Division by zero
2.0 ** 64|18446744073709551616
7.5 % 2|1.5
-7.5 % 2|-1.5
7 % 2.5|2
-7 % 3|-1
7.5 % 0|ERR:Modulo by zero
1 << -1|ERR:Negative shift count
8 >> -1|ERR:Negative shift count
1 << 64|0
-8 >> 70|-1
-8 >> 63|-1
int("9223372036854775807")|9223372036854775807
int("9223372036854775808")|ERR:does not fit in 64 bits
int("abc")|0
int("12abc")|12
float("1e999")|ERR:too large
float("1e-400")|0
type::to_int("99999999999999999999")|ERR:does not fit in 64 bits
9007199254740993 == 9007199254740992.0|false
9007199254740993 > 9007199254740992.0|true
9223372036854775807 < 9223372036854775807.0|true
(-9223372036854775807 - 1) == -9223372036854775808.0|true
1 == 1.0|true
-3 < -2.5|true
1e308 * 10|Infinity
json::encode(1e308 * 10)|ERR:Infinity cannot be written as JSON
json::encode([1.5, 2])|[1.5,2]
abs(-5)|5
math::floor(1e300)|1e+300
math::floor(-2.5)|-3
math::round(2.5)|3
math::floor(9007199254740993)|9007199254740993
int(1e19)|9223372036854775807
int(-1e300)|-9223372036854775808
int(2.9)|2
EOF
[ $fail = 0 ] && echo "PASS: numeric rules hold at their edges ($n cases)" || { echo "FAIL: numeric edge cases"; exit 1; }

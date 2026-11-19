#!/usr/bin/env bash
# No silently wrong numbers:
#   - a "0x" / "0b" literal without digits used to be 0 → compile error (Go: "literal has no digits")
#   - integer TEXT that does not fit in 64 bits used to be 0 on implicit conversion: $list["99999999999999999999"]
#     was silently element 0, bit operations gave 0 → explicit error (same rule as int() / type::to_int)
#   - text that is not a number stays loose: int("abc") == 0 (unchanged)
# Usage: numeric_overflow_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
run() { out=$(timeout 20 "$LK" -c "$1" 2>&1); rc=$?; }

run 'print(0x)'
check "0x without digits is a compile error" "$( [ $rc != 0 ] && [[ "$out" == *"hexadecimal literal 0x has no digits"* ]] && echo ok)" "rc=$rc $out"
run 'print(0b)'
check "0b without digits is a compile error" "$( [ $rc != 0 ] && [[ "$out" == *"binary literal 0b has no digits"* ]] && echo ok)" "rc=$rc $out"
run 'print(0xff + 0b101)'
check "hex and binary literals still work" "$( [ $rc = 0 ] && [ "$out" = "260" ] && echo ok)" "rc=$rc $out"

run '$a = [10, 20]
print($a["99999999999999999999"])'
check "a list index string over 64 bits is an error, not element 0" "$( [ $rc != 0 ] && [[ "$out" == *"does not fit in 64 bits"* ]] && echo ok)" "rc=$rc $out"
run 'print(6 & "99999999999999999999")'
check "a bitwise operand string over 64 bits is an error" "$( [ $rc != 0 ] && [[ "$out" == *"does not fit in 64 bits"* ]] && echo ok)" "rc=$rc $out"
run 'use type
print(type::to_int("99999999999999999999"))'
check "type::to_int over 64 bits is an error" "$( [ $rc != 0 ] && [[ "$out" == *"does not fit in 64 bits"* ]] && echo ok)" "rc=$rc $out"
run 'try { $x = int("99999999999999999999") } catch ($e) { print("caught") }'
check "the error can be caught" "$( [ $rc = 0 ] && [ "$out" = "caught" ] && echo ok)" "rc=$rc $out"

run '$a = [10, 20]
print($a["1"])
print(int("abc"))
print(int("-9223372036854775808"))
$m = ["99999999999999999999" => "id"]
print($m["99999999999999999999"])'
check "unchanged: numeric index string, int(\"abc\") == 0, int64 min, a big-number map key" "$( [ $rc = 0 ] && [ "$out" = "$(printf '20\n0\n-9223372036854775808\nid')" ] && echo ok)" "rc=$rc $out"

[ $fail = 0 ] && echo "PASS: no silent wrong numbers (0x, strings over 64 bits)" || echo "FAIL: numeric overflow"
exit $fail

#!/usr/bin/env bash
# A string holding a decimal or exponent number takes part in arithmetic as that number, on both
# engines: "2.5" + 1 is 3.5, "1e3" + 1 is 1001, -"2.5" is -2.5. It used to be read with to_int:
# "2.5" + 1 was 3 and "1e3" + 1 was 2 — a form field or a query parameter is a string, so a price or a
# quantity with a decimal was silently wrong. Everything else keeps its 1.0 behaviour (whole-number
# strings, hex, "inf", %, a leading space).
# Usage: decimal_string_arith_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
fail=0
PROG='use json
print("2.5" + 1)
print(1 + "0.5")
print("1e3" + 1)
print("2.5" * 2)
print("10" - "0.5")
print("10" / "4")
print(-"2.5")
$x = "2.5"
$x += 1
print($x)
print(json::encode(["a" => "1.5" + "1.5"]))
print("5" + 1)
print(-"5")
print("7" / "2")
print("0x10" + 1)
print("7.5" % 2)'
EXP='3.5
1.5
1001
5
9.5
2.5
-2.5
3.5
{"a":3}
6
-5
3.5
1
1'
for eng in 1 0; do
    out="$(LOOK_CLI_VM=$eng "$LK" -c "$PROG" 2>/dev/null)"
    if [ "$out" = "$EXP" ]; then echo "  OK   engine VM=$eng"; else echo "  FAIL engine VM=$eng: got [$(echo $out)]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: a decimal held in a string takes part in arithmetic as that number" || echo "FAIL: decimal string arithmetic"
exit $fail

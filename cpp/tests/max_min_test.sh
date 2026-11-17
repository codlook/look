#!/usr/bin/env bash
# max() / min() — en büyük / en küçük eleman ya da argüman, iki motorda da (VM ve tree-walk) ve
# math::max / math::min ile aynı. ESKİ: max([3,9,2]) listenin kendisi, max(1,2,8) = 2 (yalnız ilk iki).
# Usage: max_min_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
fail=0
PROG='print(max([3, 9, 2]))
print(min([3, 9, 2]))
print(max(1, 2, 8))
print(min(5, 4, 9, 1))
print(max(4, 7))
print(min(4, 7))
print(max([]) == null)
print(max(["b", "c", "a"]))
print(max([3, 9, 2]) == math::max([3, 9, 2]))
print(min(1, 2, 0) == math::min(1, 2, 0))'
EXP="$(printf '9\n2\n8\n1\n7\n4\ntrue\nc\ntrue\ntrue')"
for eng in vm tree-walk; do
    if [ $eng = vm ]; then out="$("$LK" -c "use math
$PROG" 2>&1)"; else out="$(LOOK_CLI_VM=0 "$LK" -c "use math
$PROG" 2>&1)"; fi
    if [ "$out" = "$EXP" ]; then echo "  OK   $eng"; else echo "  FAIL $eng: got [$out]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: max/min" || echo "FAIL: max/min"
exit $fail

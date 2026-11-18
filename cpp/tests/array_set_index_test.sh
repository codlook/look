#!/usr/bin/env bash
# array::set listede rakam metni anahtarı indekstir ($l["1"] = v ile aynı), iki motorda da.
# ESKİ: array::set([1,2,3], "1", "X") = {"0":1,"1":2,"2":3,"1":"X"}, count 4 — "1" iki kez.
# Usage: array_set_index_test.sh <lk>
LK="${1:?usage: $0 <lk>}"
fail=0
PROG='use array
use json
$a = array::set([1, 2, 3], "1", "X")
print(json::encode($a) . " " . count($a))
$b = array::set([1, 2, 3], "3", "Y")
print(json::encode($b) . " " . count($b))
$c = array::set([1, 2, 3], 1, "Z")
print(json::encode($c))
$d = array::set([1, 2, 3], "7", "S")
print(json::encode($d) . " " . count($d))
$l = [1, 2, 3]
$l["1"] = "X"
print(json::encode($l) == json::encode($a))'
EXP='[1,"X",3] 3
[1,2,3,"Y"] 4
[1,"Z",3]
{"0":1,"1":2,"2":3,"7":"S"} 4
true'
for eng in 1 0; do
    out="$(LOOK_CLI_VM=$eng "$LK" -c "$PROG" 2>/dev/null)"
    if [ "$out" = "$EXP" ]; then echo "  OK   engine VM=$eng"; else echo "  FAIL engine VM=$eng: got [$out]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: array::set takes a whole-number text key as an index" || echo "FAIL: array::set index"
exit $fail

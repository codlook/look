#!/usr/bin/env bash
# json::decode accepts exactly what RFC 8259 / Go's encoding/json accept (the expected column is
# Go 1.22's verdict for the same bytes) and returns null for anything else. It used to accept
# {"a" 1}, [1 2 3], [1,], text after the value, 01, "\x", raw control characters and unclosed
# strings — a proxy and LOOK could read the same body differently.
# Also: an object with 100 000 keys decodes in well under a second (each key used to be compared
# with every earlier key: 1 MB took 50 s of CPU per request — an unauthenticated DoS through
# request::json()); a duplicated key keeps its first position and takes the last value.
# Usage: json_strict_test.sh <lk>   (needs python3)
set -u
LK="$(cd "$(dirname "${1:?usage: json_strict_test.sh <lk>}")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
python3 - "$T" <<'PY'
import sys, os
T = sys.argv[1]; BS = b"\\"
V, I = "valid", "invalid"
C = [(b'{}',V), (b'[]',V), (b'{"a":1}',V), (b'[1,2,3]',V), (b'"x"',V), (b'0',V), (b'-0',V), (b'1.5',V), (b'-1.5e10',V),
     (b'1E+2',V), (b'true',V), (b'false',V), (b' {"a" : [1 , {"b":null}] } ',V),
     (b'"' + BS + b'u00e7' + BS + b'ud83d' + BS + b'ude00"',V),
     (b'"' + BS + b'/' + BS + b'b' + BS + b'f' + BS + b'n' + BS + b'r' + BS + b't' + BS + b'"' + BS + BS + b'"',V),
     (b'{"a":1,"a":2}',V), (b'9223372036854775807',V), (b'9223372036854775808',V), (b'1e308',V), (b'"\xc3\xa7"',V),
     (b'"' + BS + b'ud800"',V), (b'"' + BS + b'udc00x"',V),
     (b'{"a" 1}',I), (b'{"a":1 "b":2}',I), (b'[1 2 3]',I), (b'[1] x',I), (b'[1],',I), (b'{"a":1,}',I), (b'[1,]',I),
     (b'[,1]',I), (b'01',I), (b'-01',I), (b'1.',I), (b'.5',I), (b'1e',I), (b'+1',I), (b'0x10',I), (b'NaN',I),
     (b'Infinity',I), (b'tru',I), (b'nul',I), (b'True',I), (b"'a'",I), (b'"' + BS + b'x"',I), (b'"' + BS + b'u12"',I),
     (b'"' + BS + b'u12zz"',I), (b'"abc',I), (b'"a\x01b"',I), (b'"a\nb"',I), (b'"\t"',I), (b'"a' + BS + b'"',I),
     (b'{a:1}',I), (b'{"a"}',I), (b'{"a":}',I), (b'[',I), (b'{',I), (b'',I), (b'   ',I), (b'1e999',I),
     (b'[1]\x00',I), (b'{"a":1}}',I), (b'[[1]]]',I), (b'\xef\xbb\xbf{}',I), (b'/*c*/{}',I)]
with open(os.path.join(T, "list.txt"), "w") as L:
    for n, (b, want) in enumerate(C):
        open(os.path.join(T, "c%d.json" % n), "wb").write(b)
        L.write("%d %s %r\n" % (n, want, b[:30]))
n = 100000
open(os.path.join(T, "wide.json"), "w").write("{" + ",".join('"k%d":%d' % (i, i) for i in range(n)) + ',"k5":-1}')
PY
cat > "$T/one.lk" <<'LK'
use file
$v = json::decode(file::read(args()[0]))
print(($v == null) ? "NULL" : json::encode($v))
LK
fail=0
while read -r n want label; do
    out="$(cd "$T" && "$LK" one.lk "c$n.json" 2>&1 | grep -v '^\[20' | tail -1)"
    got=valid; [ "$out" = NULL ] && got=invalid
    if [ "$got" = "$want" ]; then :; else echo "  FAIL $label: want $want, got $got ($out)"; fail=1; fi
done < "$T/list.txt"
[ $fail = 0 ] && echo "  OK   $(wc -l < "$T/list.txt") inputs: the same valid/invalid verdict as Go's encoding/json"
cat > "$T/wide.lk" <<'LK'
use file
$d = json::decode(file::read("wide.json"))
print(count($d) . " " . $d["k5"] . " " . $d["k99999"])
LK
st=$(date +%s%N); out="$(cd "$T" && timeout 60 "$LK" wide.lk 2>&1 | tail -1)"; ms=$(( ($(date +%s%N) - st) / 1000000 ))
if [ "$out" = "100000 -1 99999" ] && [ $ms -lt 2000 ]; then echo "  OK   100 000 keys in ${ms} ms; a duplicated key takes the last value in its first position"
else echo "  FAIL 100 000 keys: '$out' in ${ms} ms (want '100000 -1 99999' under 2000 ms)"; fail=1; fi
[ $fail = 0 ] && echo "PASS: json::decode is strict and linear" || { echo "FAIL: json::decode"; exit 1; }

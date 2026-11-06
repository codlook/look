#!/usr/bin/env bash
# MySQL and PostgreSQL clients against a hostile server: thousands of mutated replies (flipped,
# cut, extended, dropped or repeated packets, broken lengths and type bytes, extra result sets,
# errors between rows). Every query must end — with rows or an error — never a crash, a hang or
# (under ASan) a memory error. Covers what the small parser fuzz targets cannot reach: buffered
# reads, packet reassembly, binary/text rows, CALL's extra results, COPY replies.
# Usage: db_wire_mutation_test.sh <lk> [queries per driver] [seed]   (needs python3; run with an ASan lk)
set -u
LK="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
N="${2:-1500}"; SEED="${3:-1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; s1=""; s2=""; trap '[ -n "$s1" ] && kill $s1 2>/dev/null; [ -n "$s2" ] && kill $s2 2>/dev/null; rm -rf "$T"' EXIT
MP="${MP:-33111}"; PP="${PP:-55111}"
python3 "$HERE/db_wire_mutator.py" mysql "$MP" "$SEED" > "$T/my.log" 2>&1 & s1=$!
python3 "$HERE/db_wire_mutator.py" pg "$PP" "$SEED" > "$T/pg.log" 2>&1 & s2=$!
for p in $MP $PP; do for i in $(seq 1 50); do (echo > "/dev/tcp/127.0.0.1/$p") 2>/dev/null && break; sleep 0.1; done; done
fail=0
for drv in mysql pg; do
    if [ $drv = mysql ]; then dsn="mysql://u:p@127.0.0.1:$MP/d"; else dsn="postgres://u:p@127.0.0.1:$PP/d"; fi
    cat > "$T/q.lk" <<LK
use db
\$c = db::connect("$dsn")
\$rows = 0
\$errs = 0
for (\$i = 0; \$i < $N; \$i++) {
    try { \$r = db::query(\$c, "SELECT * FROM t WHERE id = ?", [\$i]); \$rows = \$rows + count(\$r) }
    catch (\$e) { \$errs = \$errs + 1 }
}
print("done rows=" . \$rows . " errors=" . \$errs)
LK
    st=$(date +%s)
    out="$(cd "$T" && timeout 600 "$LK" q.lk 2>&1)"; rc=$?
    dt=$(( $(date +%s) - st ))
    last="$(echo "$out" | grep -v '^\[20' | tail -1)"
    if [ $rc = 0 ] && echo "$last" | grep -q "^done "; then
        echo "  OK   [$drv] $N mutated replies: $last (${dt} s)"
    else
        echo "  FAIL [$drv] rc=$rc after ${dt} s: $(echo "$out" | grep -E 'ERROR: AddressSanitizer|runtime error|SUMMARY|Segmentation|Abort' | head -3) $last"
        echo "$out" | grep -A20 "ERROR: AddressSanitizer" | head -30
        fail=1
    fi
done
[ $fail = 0 ] && echo "PASS: database clients survive hostile replies" || { echo "FAIL: database clients against hostile replies"; exit 1; }

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
# Sahte sunucu dinlemeye başlayamazsa (port geçici aralıkta başka bir soketle dolu: bind hatası)
# başka portla yeniden dene — yoksa istemci "bağlanamadı" der ve bu, ayrıştırıcı bulgusu sanılırdı.
start_mutator() {   # proto port log → yazdırır: pid port
    local proto=$1 port=$2 log=$3 pid
    for try in 1 2 3 4 5; do
        python3 "$HERE/db_wire_mutator.py" "$proto" "$port" "$SEED" > "$log" 2>&1 & pid=$!
        for i in $(seq 1 100); do grep -q "^mutator" "$log" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.05; done
        if kill -0 $pid 2>/dev/null && grep -q "^mutator" "$log"; then echo "$pid $port"; return 0; fi
        echo "  ..   harness: $proto mutator could not listen on $port ($(tail -1 "$log")); trying another port" >&2
        port=$((port + 1000))
    done
    return 1
}
read -r s1 MP <<< "$(start_mutator mysql "$MP" "$T/my.log")" || { echo "FAIL: harness: the mysql mutator did not start"; exit 2; }
read -r s2 PP <<< "$(start_mutator pg "$PP" "$T/pg.log")" || { echo "FAIL: harness: the pg mutator did not start"; exit 2; }
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
        echo "  ..   client output (last lines):"; echo "$out" | grep -v '^\[20' | tail -6 | sed 's/^/         /'
        kill -0 $s1 2>/dev/null && kill -0 $s2 2>/dev/null || echo "  ..   harness: a mutator process is gone"
        fail=1
    fi
done
[ $fail = 0 ] && echo "PASS: database clients survive hostile replies" || { echo "FAIL: database clients against hostile replies"; exit 1; }

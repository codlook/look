#!/usr/bin/env bash
# db_stateful_test.sh — PG wire-parser DURUM-DİZİLİ dayanıklılık guard'ı (kategori 1).
#
# Sahte-PG sunucusu durum-dizili/sırasız yanıtlar gönderir; istemci ÇÖKMEDEN
# (segfault/OOM/NULL-deref) nazikçe işlemeli. 53. bug (commit_kopar) bu harness'ten
# çıktı; bu guard kalan durum-dizili senaryoları kapatır:
#   sirasiz     — DataRow, RowDescription'DAN ÖNCE (columns boş → default, deref yok)
#   erken_c     — CommandComplete, hiç DataRow/RowDescription olmadan
#   hs_error    — handshake'te AuthenticationOk yerine ErrorResponse
#   param_seli  — 200000 ParameterStatus seli (bellek sınırlı mı?)
set -u
LK="${1:-./build/lk}"
PY="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
i=0
for MODE in sirasiz erken_c hs_error param_seli; do
    PORT=$((55490+i)); i=$((i+1))
    "$PY" "$HERE/fake_pg_server.py" "$MODE" "$PORT" >/dev/null 2>&1 &
    FPS=$!
    sleep 1
    cat > "$TMP/q.lk" <<LK
use db
try {
    \$c = db::connect("postgres://u:p@127.0.0.1:$PORT/d")
    db::query(\$c, "SELECT 1")
    print("SURVIVED")
} catch (\$e) { print("SURVIVED-CAUGHT") }
LK
    out=$(timeout 40 "$LK" "$TMP/q.lk" 2>&1 | grep -v "INFO\|Pool")
    rc=$?
    kill "$FPS" 2>/dev/null
    if [ "$rc" -ge 128 ]; then
        echo "  FAIL [$MODE]: istemci ÇÖKTÜ (exit=$rc; 139=segfault, 137=OOM)"; fail=1
    elif echo "$out" | grep -q "SURVIVED"; then
        echo "  PASS [$MODE]: nazikçe işlendi (çökme yok)"
    else
        echo "  FAIL [$MODE]: SURVIVED çıktısı yok (exit=$rc): $(echo "$out" | tr '\n' ' ' | head -c 80)"; fail=1
    fi
done
[ $fail = 0 ] && echo "PASS: db stateful (kategori 1 — wire-parser durum-dizili dayaniklilik)" || echo "FAIL: db stateful"
exit $fail

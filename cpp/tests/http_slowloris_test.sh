#!/usr/bin/env bash
# http_slowloris_test.sh — Slowloris savunma guard'i (56. bug).
# Header'lar bir istek basladiktan sonra toplam sure icinde tamamlanmali;
# yavas-damla baglanti tutamamali. Dusuk LOOK_HEADER_TIMEOUT ile hizli test.
set -u
LKFCGI="${1:-./build/lk-fcgi}"; HERE="$(cd "$(dirname "$0")" && pwd)"; PORT="${2:-7622}"
LOOK_HEADER_TIMEOUT=2000 "$LKFCGI" --mode http --port "$PORT" --workers 2 "$HERE/http_echo_app.lk" >/tmp/slw.log 2>&1 &
P=$!; sleep 2
python3 "$HERE/http_slowloris_test.py" "$PORT"; rc=$?
kill "$P" 2>/dev/null
[ $rc = 0 ] && echo "PASS: http slowloris (56 — header total deadline)" || echo "FAIL: http slowloris"
exit $rc

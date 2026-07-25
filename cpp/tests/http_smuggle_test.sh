#!/usr/bin/env bash
# http_smuggle_test.sh — HTTP istek-smuggling / RFC 7230 çerçeve guard'ı (54. bug).
# TE substring eşlemesi (xchunked→chunked) ve field-name'den önce boşluk
# (Content-Length :) request smuggling'e zemindi. Bu guard geçersiz çerçeve
# başlıklarının REDDEDILDIGINI, geçerlilerin işlendiğini doğrular.
set -u
LKFCGI="${1:-./build/lk-fcgi}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${2:-7603}"
"$LKFCGI" --mode http --port "$PORT" --workers 2 "$HERE/http_echo_app.lk" >/tmp/hsm.log 2>&1 &
P=$!
sleep 2
python3 "$HERE/http_smuggle_test.py" "$PORT"; rc=$?
kill "$P" 2>/dev/null
[ $rc = 0 ] && echo "PASS: http smuggle (54 — TE token + field-name bosluk)" || echo "FAIL: http smuggle"
exit $rc

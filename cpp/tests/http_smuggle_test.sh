#!/usr/bin/env bash
# http_smuggle_test.sh — HTTP request-smuggling / RFC 7230 framing guard (bug 54).
# TE substring matching (xchunked→chunked) and whitespace before the field-name
# (Content-Length :) were the ground for request smuggling. This guard verifies that invalid framing
# headers are REJECTED and valid ones are processed.
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

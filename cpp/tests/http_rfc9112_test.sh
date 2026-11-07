#!/usr/bin/env bash
# HTTP/1.1 request parsing boundary table (RFC 9112, Go net/http as the reference) against
# lk-fcgi --mode http, in both dispatch modes (fiber and worker pool).
# Usage: http_rfc9112_test.sh <lk-fcgi> [port] [go_port]
set -u
LKFCGI="$(cd "$(dirname "${1:?usage: http_rfc9112_test.sh <lk-fcgi> [port] [go_port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7796}"; GOPORT="${3:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; P=""; trap '[ -n "$P" ] && kill $P 2>/dev/null; rm -rf "$T"' EXIT
cp "$HERE/http_rfc9112_app.lk" "$T/app.lk"
rc=0
for mode in fiber pool; do
    if [ $mode = pool ]; then export LOOK_FIBER_DISPATCH=0; else unset LOOK_FIBER_DISPATCH; fi
    (cd "$T" && LOOK_SECRET=rfc9112-test-secret-0123456789abcdef0123 exec "$LKFCGI" --mode http --port "$PORT" --workers 2 app.lk) > "$T/srv_$mode.log" 2>&1 &
    P=$!
    for i in $(seq 1 100); do
        python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" 2>/dev/null && break
        sleep 0.1
    done
    echo "== $mode"
    python3 "$HERE/http_rfc9112_test.py" "$PORT" $GOPORT || rc=1
    kill $P 2>/dev/null; wait $P 2>/dev/null; P=""
done
[ $rc = 0 ] && echo "PASS: RFC 9112 request parsing (fiber + pool)" || echo "FAIL: RFC 9112 request parsing"
exit $rc

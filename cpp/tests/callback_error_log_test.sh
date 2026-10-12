#!/usr/bin/env bash
# An error in a WebSocket / SSE connection callback is written to the log.
#
# The client of such a callback gets no response to carry the error, so the log is the only
# place it can show. Up to 1.0.15 the WebSocket `message` and `close` callbacks and the SSE
# `close` callback caught every error and dropped it: a message handler that failed did
# nothing, and nothing said why (measured on the published 1.0.15, both engines: 0 lines).
# Usage: callback_error_log_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7795}"
cat > "$TMP/app.lk" <<'LK'
route("WS", "/ws", function($ws) {
    ws::on($ws, "message", function($m) { $x = 10 / 0; ws::send($ws, "never") })
    ws::on($ws, "close", function() { throw "close-boom" })
})
route("SSE", "/sse", function($sse) { sse::send($sse, "x"); sse::on($sse, "close", function() { throw "sse-close-boom" }) })
route("GET", "/ok", fn() => response::text("alive"))
LK
cat > "$TMP/client.py" <<'PY'
import socket, os, sys, time
port = int(sys.argv[1])
s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", port))
s.sendall(b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"); s.recv(4096)
m = os.urandom(4); p = b"ping"; s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))
time.sleep(0.7); s.close()
e = socket.socket(); e.settimeout(3); e.connect(("127.0.0.1", port)); e.sendall(b"GET /sse HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"); e.recv(4096); time.sleep(0.3); e.close()
time.sleep(1.0)
PY
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    python3 "$TMP/client.py" "$PORT"
    alive="$(curl -s -m 3 "localhost:$PORT/ok")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    m="$(grep -c 'ws message.*Division by zero' "$TMP/log.txt")"; c="$(grep -c 'ws close.*close-boom' "$TMP/log.txt")"; s="$(grep -c 'sse close.*sse-close-boom' "$TMP/log.txt")"
    if [ "$m" = 1 ] && [ "$c" = 1 ] && [ "$s" = 1 ] && [ "$alive" = alive ]; then echo "  OK   [$mode] the three callback errors are in the log, the server answers"
    else echo "  FAIL [$mode] logged: ws message=$m ws close=$c sse close=$s (want 1 each), server answers: [${alive:-nothing}]"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: connection callback errors are logged" || { echo "FAIL: a connection callback error left no trace"; exit 1; }

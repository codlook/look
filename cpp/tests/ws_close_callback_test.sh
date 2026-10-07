#!/usr/bin/env bash
# The "close" callback of a WebSocket runs when the client goes away — with or without a
# close frame.
#
# It ran only when the client sent a close frame. A client that just disappears (network loss,
# a closed tab, a phone going to sleep) sends none: the callback never ran, and whatever the
# application keeps per connection (a presence list, a room membership) was never cleaned up.
# An SSE connection already reported a dropped client. The rule, same for both:
#   * the client goes away (close frame or dropped connection)  -> the callback runs, once
#   * the server closes the connection itself (ws::close / sse::close) -> it does not
# Usage: ws_close_callback_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7731}"
cat > "$TMP/app.lk" <<'LK'
use json
use cache
function n($k) { return cache::get("c_" . $k) ?? 0 }
route("WS", "/ws/{k}", function($ws, $k) {
    ws::on($ws, "close", function() { cache::set("c_" . $k, n($k) + 1, 60) })
    ws::on($ws, "message", function($m) { if ($m == "bye") { ws::close($ws) } })
})
route("SSE", "/sse/{k}", function($sse, $k) {
    sse::on($sse, "close", function() { cache::set("c_" . $k, n($k) + 1, 60) })
    sse::send($sse, "x")
    if ($k == "ssesrv") { sse::close($sse) }
})
route("GET", "/r", fn() => response::json(["frame" => n("frame"), "drop" => n("drop"), "frame_then_drop" => n("both"),
      "server_close" => n("bye"), "sse_drop" => n("ssecli"), "sse_server_close" => n("ssesrv"), "open" => ws::clients()]))
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, time, sys
port = int(sys.argv[1])
def frame(p, op=0x81):
    m = os.urandom(4); return bytes([op, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
def conn(k):
    s = socket.create_connection(("127.0.0.1", port)); key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws/%s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (k, key)).encode())
    time.sleep(0.3); s.recv(4096); return s
s = conn("frame"); s.sendall(frame(b"", 0x88)); time.sleep(0.4); s.close()      # close frame, then wait
s = conn("drop"); s.close()                                                     # no close frame
s = conn("both"); s.sendall(frame(b"", 0x88)); s.close()                        # close frame and drop at once
s = conn("bye"); s.sendall(frame(b"bye")); time.sleep(0.4); s.close()           # the server closes
PY
WANT='{"frame":1,"drop":1,"frame_then_drop":1,"server_close":0,"sse_drop":1,"sse_server_close":0,"open":0}'
fail=0
for mode in "LOOK_X=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    python3 "$TMP/client.py" "$PORT"
    curl -s -m 1 -N -H 'Accept: text/event-stream' "localhost:$PORT/sse/ssecli" >/dev/null 2>&1
    curl -s -m 1 -N -H 'Accept: text/event-stream' "localhost:$PORT/sse/ssesrv" >/dev/null 2>&1
    sleep 0.8
    got="$(curl -s -m 3 "localhost:$PORT/r")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = "$WANT" ]; then echo "  OK   [$mode] close runs once when the client goes away, not when the server closes"
    else echo "  FAIL [$mode] $got"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: the close callback runs when the client goes away" || { echo "FAIL: close callback"; exit 1; }

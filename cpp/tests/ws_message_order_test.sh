#!/usr/bin/env bash
# The messages of one WebSocket connection reach its callback in order, one at a time, and the
# close callback runs after the last of them.
#
# Every incoming message was handed to the worker pool as a separate job: two messages of the
# same connection ran at the same time on different threads and could finish in reverse order.
# Measured before the fix: of 2000 messages sent back to back, ~10% were echoed out of order and
# the callback overlapped itself ~250 times. A chat could show messages in the wrong order, and
# state kept per connection raced with itself. Connections still run in parallel with each other.
# Usage: ws_message_order_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7746}"; N="${N:-2000}"
cat > "$TMP/app.lk" <<'LK'
use cache
route("WS", "/ws/{id}", function($ws, $id) {
    ws::on($ws, "message", function($m) {
        $busy = cache::get("busy" . $id) ?? 0
        cache::set("busy" . $id, $busy + 1, 60)
        if ($busy > 0) { cache::set("overlap", (cache::get("overlap") ?? 0) + 1, 60) }
        $x = 0
        for ($i = 0; $i < 200; $i++) { $x = $x + $i }
        cache::set("busy" . $id, (cache::get("busy" . $id) ?? 1) - 1, 60)
        cache::set("seen" . $id, (cache::get("seen" . $id) ?? 0) + 1, 60)
        ws::send($ws, $m)
    })
    ws::on($ws, "close", function() { cache::set("at_close" . $id, cache::get("seen" . $id) ?? 0, 60) })
})
// No echo: a client that closes after sending leaves nothing unread, so it closes with FIN (not RST)
// and every byte it sent reaches the server.
route("WS", "/quiet/{id}", function($ws, $id) {
    ws::on($ws, "message", function($m) { cache::set("seen" . $id, (cache::get("seen" . $id) ?? 0) + 1, 60) })
    ws::on($ws, "close", function() { cache::set("at_close" . $id, cache::get("seen" . $id) ?? 0, 60) })
})
route("GET", "/clients", fn() => response::text(ws::clients()))
route("GET", "/overlap", fn() => response::text(cache::get("overlap") ?? 0))
route("GET", "/at_close/{id}", function($id) { return response::text(cache::get("at_close" . $id) ?? "none") })
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, sys, threading, struct
port, N = int(sys.argv[1]), int(sys.argv[2])
def frame(p, op=0x81):
    m = b"\x01\x02\x03\x04"; return bytes([op, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
def conn(cid, out, close_after=False, path="/ws", rst=False, count=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=20)
    s.sendall(("GET %s/%s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, cid, base64.b64encode(os.urandom(16)).decode())).encode())
    d = b""
    while b"\r\n\r\n" not in d: d += s.recv(4096)
    buf = d.split(b"\r\n\r\n", 1)[1]
    s.sendall(b"".join(frame(str(i).encode()) for i in range(1, (count or N) + 1)))
    if rst:   # leave without reading the echoes and without a close frame: the kernel sends RST
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); s.close(); return
    if close_after:
        s.sendall(frame(b"", 0x88)); s.close(); return
    got = []
    while len(got) < N:
        while len(buf) < 2: buf += s.recv(65536)
        n = buf[1] & 0x7f
        while len(buf) < 2 + n: buf += s.recv(65536)
        got.append(int(buf[2:2 + n])); buf = buf[2 + n:]
    s.close()
    out[cid] = sum(1 for a, b in zip(got, got[1:]) if b < a)
res = {}
ts = [threading.Thread(target=conn, args=("c%d" % i, res)) for i in range(4)]
[t.start() for t in ts]; [t.join() for t in ts]
print(sum(res.values()), len(res))
conn("closer", {}, close_after=True, path="/quiet")
conn("rst", {}, rst=True, count=20000)   # enough to fill the inbox and pause reading
PY
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/overlap" && break; sleep 0.1; done
fail=0
read -r inv conns <<< "$(python3 "$TMP/client.py" "$PORT" "$N")"
[ "$conns" = 4 ] && [ "$inv" = 0 ] && echo "  OK   4 connections x $N messages: every echo in order" \
    || { echo "  FAIL out of order: $inv (connections answered: $conns)"; fail=1; }
ov="$(curl -s "localhost:$PORT/overlap")"
[ "$ov" = 0 ] && echo "  OK   a connection's callback never ran twice at the same time" \
    || { echo "  FAIL a connection's callback overlapped itself $ov times"; fail=1; }
for i in $(seq 1 100); do ac="$(curl -s "localhost:$PORT/at_close/closer")"; [ "$ac" != none ] && break; sleep 0.1; done
[ "$ac" = "$N" ] && echo "  OK   the close callback ran after all $N messages" \
    || { echo "  FAIL the close callback saw $ac of $N messages"; fail=1; }
# A client that resets the connection while its reads are paused (inbox full) and its echoes fail:
# the connection must still be closed — close callback once, no connection left open.
for i in $(seq 1 100); do ar="$(curl -s "localhost:$PORT/at_close/rst")"; [ "$ar" != none ] && break; sleep 0.1; done
[ "$ar" != none ] && echo "  OK   a client that reset mid-flood got its close callback (after $ar messages)" \
    || { echo "  FAIL no close callback for a client that reset mid-flood"; fail=1; }
for i in $(seq 1 50); do nc="$(curl -s "localhost:$PORT/clients")"; [ "$nc" = 0 ] && break; sleep 0.1; done
[ "$nc" = 0 ] && echo "  OK   no WebSocket left open" || { echo "  FAIL $nc WebSocket(s) still open"; fail=1; }
kill -0 "$pid" 2>/dev/null && echo "  OK   the server is still running" || { echo "  FAIL the server died"; fail=1; }
[ $fail = 0 ] && echo "PASS: a WebSocket connection's messages are handled in order, one at a time" \
             || { echo "FAIL: WebSocket message order"; exit 1; }

#!/usr/bin/env bash
# WebSocket close + fd-leak functional guard (twin of SSE sse_close_test.sh, without TSan).
# Raw-socket handshake (python3 stdlib — CI has no websocket-client) + a /proc/PID/fd counter.
# Scope: handshake 101 · echo · client close frame → server close + fd freed · regression.
# NOTE: WS EWOULDBLOCK (slow-client write drop) is OUT OF SCOPE for this functional test — it needs a separate
# load/backpressure test (send_raw is blocking; on record).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7769}"
TMP=$(mktemp -d)
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 required)"; exit 0; }
[ -d /proc ] || { echo "  (skipped: /proc required)"; exit 0; }
fail=0

cat > "$TMP/app.lk" <<'LKEOF'
route("WS", "/ws", function($conn) {
    ws::on($conn, "message", function($m) {
        if ($m == "bye") { ws::close($conn) }   // SERVER-side close
        else { ws::send($conn, "echo:" . $m) }
    })
})
route("GET", "/", function() { print("ok") })
LKEOF

"$FCGI" --mode http --port "$PORT" "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null && break; sleep 0.25; done
sleep 1
base=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# Raw-socket WS client: handshake→echo→close; fds are counted while connected and after close.
out=$(PORT="$PORT" python3 - "$SRV" "$base" <<'PYEOF'
import socket, os, base64, struct, sys, time
pid, base = sys.argv[1], int(sys.argv[2])
port = int(os.environ["PORT"])
def fdcount():
    try: return len(os.listdir("/proc/%s/fd" % pid))
    except Exception: return -1
def masked(op, payload=b""):
    m = os.urandom(4)
    mp = bytes(b ^ m[i % 4] for i, b in enumerate(payload))
    return bytes([0x80 | op, 0x80 | len(payload)]) + m + mp
s = socket.create_connection(("127.0.0.1", port), timeout=5); s.settimeout(5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
resp = s.recv(1024)
ok101 = b"101" in resp.split(b"\r\n")[0]
s.sendall(masked(0x1, b"ping"))                 # text frame
fr = s.recv(256)                                # server echo (unmasked)
echoed = b"echo:ping" in fr
time.sleep(0.3); conn = fdcount()               # fd while connected
s.sendall(masked(0x8))                          # close frame
try: tail = s.recv(256)                          # server close frame + EOF
except Exception: tail = b""
s.close(); time.sleep(1); after = fdcount()
print("OK101=%s ECHO=%s CONN=%d BASE=%d AFTER=%d" % (ok101, echoed, conn, base, after))
PYEOF
)
echo "  $out"
ok101=$(echo "$out" | grep -o 'OK101=True'); echoed=$(echo "$out" | grep -o 'ECHO=True')
conn=$(echo "$out" | sed -n 's/.*CONN=\([0-9]*\).*/\1/p'); after=$(echo "$out" | sed -n 's/.*AFTER=\([0-9]*\).*/\1/p')

# Sanity (Rule 3): did the handshake + echo really happen (did the test exercise the WS path)
if [ -z "$ok101" ] || [ -z "$echoed" ] || [ "${conn:-0}" -ne $((base+1)) ]; then
  echo "  FAIL sanity: WS did not connect / no echo (fd $base->${conn:-?})"; fail=1
else echo "  OK handshake 101 + echo:ping + connected fd=$conn"; fi
# The point: did the fd count return to base after close (no leak)
if [ "${after:-99}" -eq "$base" ]; then echo "  OK fd returned to base after close ($after) — no leak"
else echo "  FAIL: fd=$after after close (base=$base) — LEAK"; fail=1; fi

# SERVER-side close (ws::close): the client sends "bye", the SERVER closes → the client gets a close frame
# (opcode 0x8) and the fd count returns to base (twin of SSE server-close). The client does NOT initiate a close frame.
sout=$(PORT="$PORT" python3 - "$SRV" "$base" <<'PYEOF'
import socket, os, base64, sys, time
pid, base = sys.argv[1], int(sys.argv[2])
port = int(os.environ["PORT"])
def fdcount():
    try: return len(os.listdir("/proc/%s/fd" % pid))
    except Exception: return -1
def masked(op, payload=b""):
    m = os.urandom(4)
    mp = bytes(b ^ m[i % 4] for i, b in enumerate(payload))
    return bytes([0x80 | op, 0x80 | len(payload)]) + m + mp
s = socket.create_connection(("127.0.0.1", port), timeout=5); s.settimeout(5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
r = s.recv(1024); ok101 = b"101" in r.split(b"\r\n")[0]
s.sendall(masked(0x1, b"bye"))                  # the server calls ws::close on this
got_close = False
try:
    fr = s.recv(256)                             # the server's close frame (unmasked, opcode 0x8)
    if fr and (fr[0] & 0x0F) == 0x8: got_close = True
except Exception: pass
s.close(); time.sleep(1); after = fdcount()
print("OK101=%s SRVCLOSE=%s AFTER=%d BASE=%d" % (ok101, got_close, after, base))
PYEOF
)
echo "  $sout"
sok101=$(echo "$sout" | grep -o 'OK101=True'); srvclose=$(echo "$sout" | grep -o 'SRVCLOSE=True')
safter=$(echo "$sout" | sed -n 's/.*AFTER=\([0-9]*\).*/\1/p')
if [ -z "$sok101" ] || [ -z "$srvclose" ]; then
  echo "  FAIL server-close: ws::close did not send a close frame to the client"; fail=1
else echo "  OK ws::close -> the client received a close frame (server-initiated)"; fi
if [ "${safter:-99}" -eq "$base" ]; then echo "  OK fd returned to base after server-close ($safter)"
else echo "  FAIL: fd=$safter after server-close (base=$base) — LEAK"; fail=1; fi

# A client that does NOT ANSWER after ws::close: the server must close the connection itself (the fd and
# the connection used to stay open indefinitely until the client sent something).
qout=$(PORT="$PORT" python3 - <<'PYEOF'
import socket, os, base64, time
port = int(os.environ["PORT"])
def masked(op, payload=b""):
    m = os.urandom(4)
    return bytes([0x80 | op, 0x80 | len(payload)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(payload))
s = socket.create_connection(("127.0.0.1", port), timeout=5); s.settimeout(3)
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
s.recv(1024); s.sendall(masked(0x1, b"bye"))
data = b""; eof = False; t0 = time.time()
try:
    while True:
        c = s.recv(256)
        if not c: eof = True; break
        data += c
except socket.timeout: pass
print("EOF=%s IN=%.1f" % (eof, time.time() - t0))
PYEOF
)
echo "  $qout"
if echo "$qout" | grep -q 'EOF=True'; then echo "  OK ws::close: the server closed the connection of a non-responding client"
else echo "  FAIL ws::close: the server did not close the connection (it stays open when the client does not respond)"; fail=1; fi

# Regression: 3 abrupt disconnects (abandon without a close frame) → no leak/crash
python3 - "$PORT" <<'PYEOF'
import socket, os, base64
port = int(__import__("sys").argv[1])
for _ in range(3):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    s.recv(256); s.close()   # abrupt disconnect
PYEOF
sleep 1
recyc=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$SRV" 2>/dev/null && [ "$recyc" -le $((base+1)) ]; then echo "  OK abrupt-disconnect 3 cycles: fd=$recyc, server alive"
else echo "  FAIL: abrupt-disconnect regression (fd=$recyc, server $(kill -0 $SRV 2>/dev/null && echo alive || echo CRASHED))"; fail=1; fi

[ $fail = 0 ] && echo "PASS: WS close + fd leak" || echo "FAIL: WS close + fd leak"
exit $fail

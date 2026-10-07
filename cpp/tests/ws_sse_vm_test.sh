#!/usr/bin/env bash
# LOOK 2: ordinary routes that use ws:: / sse:: run on the VM.
#
# ws::send, ws::close, ws::broadcast, ws::clients, sse::send, sse::close and sse::clients
# were known only to the tree-walk engine, so a plain route such as
#     route("POST", "/notify", function() { ws::broadcast("...") })
# was placed on the tree-walk engine at startup. They are VM builtins now.
# The WS and SSE route handlers themselves, and the callbacks given to ws::on / sse::on, still
# run on the tree-walk engine on the connection's thread; this test also pins down what must
# hold there:
#   * a broadcast from a VM route reaches an open connection
#   * a message handler runs once per message, also when it fails; the error is logged
#     (it used to be swallowed) and the connection keeps working
# That a handler's write to a global reaches no later request is NOT checked here: the handler
# runs on the tree-walk engine and this test reads through VM routes, which have their own
# globals, so a leak on that side would not show. tests/realtime_isolation_test.sh checks it in
# the mode where it is visible (it turns red when the 1.0.5 fix is removed; this test does not).
# Usage: ws_sse_vm_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7728}"
cat > "$TMP/app.lk" <<'LK'
use json
use cache
function seen() { return cache::get("seen") ?? 0 }
function on_msg($m) { cache::set("seen", seen() + 1, 60); cache::set("last", $m, 60); return 1 }
route("GET", "/notify", function() { ws::broadcast("hello"); return response::json(["ws" => ws::clients(), "sse" => sse::clients()]) })
route("WS", "/ws", function($ws) {
    ws::on($ws, "message", function($m) { on_msg($m); if ($m == "boom") { throw "boom in ws" } })
})
route("SSE", "/sse", function($sse) { sse::send($sse, "x"); sse::close($sse) })
route("GET", "/r", fn() => response::json(["seen" => seen(), "last" => cache::get("last") ?? "none"]))
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, sys, time, urllib.request
port = int(sys.argv[1])
def frame(p):
    m = os.urandom(4); return bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
s = socket.create_connection(("127.0.0.1", port)); k = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % k).encode())
time.sleep(0.3); s.recv(4096)
notify = urllib.request.urlopen("http://127.0.0.1:%d/notify" % port, timeout=3).read().decode()
s.settimeout(2)
try: got = s.recv(4096)
except Exception: got = b""
s.sendall(frame(b"m1")); time.sleep(0.3)
s.sendall(frame(b"boom")); time.sleep(0.3)
s.sendall(frame(b"m3")); time.sleep(0.4)
s.close()
print(notify + " broadcast=" + ("yes" if b"hello" in got else "no"))
PY
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    c="$(python3 "$TMP/client.py" "$PORT" 2>&1 | tail -1)"
    sse="$(curl -s -m 2 -N -H 'Accept: text/event-stream' "localhost:$PORT/sse" 2>/dev/null | tr -d '\r\n')"
    r="$(curl -s -m 3 "localhost:$PORT/r")"
    placed="$(grep -c 'Route GET:/notify runs on the interpreter\|VM BUG\|interpreter fallback' "$TMP/log.txt")"
    errlog="$(grep -c 'message handler error: boom in ws' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$c" = '{"ws":1,"sse":0} broadcast=yes' ] && [ "$placed" = "0" ]; then echo "  OK   [$mode] /notify runs on the VM; its broadcast reached the open connection"
    else echo "  FAIL [$mode] client: $c; notify placed on the interpreter / fallback lines: $placed"; fail=1; fi
    if [ "$r" = '{"seen":3,"last":"m3"}' ] && [ "$errlog" = "1" ]; then echo "  OK   [$mode] three messages handled once each, the failing one logged"
    else echo "  FAIL [$mode] r=$r, logged handler errors: $errlog"; fail=1; fi
    case "$sse" in *"data: x"*) echo "  OK   [$mode] the SSE route sent its event";; *) echo "  FAIL [$mode] SSE output: [$sse]"; fail=1;; esac
done
[ $fail = 0 ] && echo "PASS: routes using ws:: and sse:: run on the VM; connection handlers behave" || { echo "FAIL: ws/sse on the VM"; exit 1; }

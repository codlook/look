#!/usr/bin/env bash
# LOOK 2: WebSocket and SSE run on the VM — the routes, the connection handlers and the
# callbacks given to ws::on / sse::on.
#
# They used to run on the tree-walk engine: a WS or SSE route was dispatched there directly,
# and any ordinary route that called ws::broadcast / sse::send was placed there at startup.
# Now a connection handler is dispatched like a request (the connection is its first argument,
# path parameters follow by name), and a callback runs like a new request: its own VM, the
# globals at their declared values. This test pins down:
#   * no route is placed on the tree-walk engine; it also works with LOOK_VM_STRICT=1
#   * a broadcast from an ordinary route reaches an open connection
#   * a WS route receives its path parameter by name; the callback sees the captured values
#   * a message callback runs once per message, also when it fails; the error is logged and
#     the connection keeps working
#   * the close callbacks of ws::on and sse::on run
# That a handler's write to a global reaches no later request: tests/realtime_isolation_test.sh.
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
route("WS", "/ws/{room}", function($ws, $room) {
    $tag = "t-" . $room
    ws::on($ws, "message", function($m) {
        on_msg($m)
        if ($m == "boom") { throw "boom in ws" }
        ws::send($ws, "echo:" . $m . ":" . $room . ":" . $tag)
    })
    ws::on($ws, "close", function() { cache::set("ws_closed", $room, 60) })
})
route("SSE", "/sse", function($sse) {
    sse::on($sse, "close", function() { cache::set("sse_closed", "yes", 60) })
    sse::send($sse, "x")
})
route("GET", "/r", fn() => response::json(["seen" => seen(), "last" => cache::get("last") ?? "none",
      "ws_closed" => cache::get("ws_closed") ?? "no", "sse_closed" => cache::get("sse_closed") ?? "no"]))
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, sys, time, urllib.request
port = int(sys.argv[1])
def frame(p):
    m = os.urandom(4); return bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
s = socket.create_connection(("127.0.0.1", port)); k = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws/r7 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % k).encode())
time.sleep(0.3); s.recv(4096)
notify = urllib.request.urlopen("http://127.0.0.1:%d/notify" % port, timeout=3).read().decode()
s.settimeout(2)
def rd():
    try: return s.recv(4096)
    except Exception: return b""
got = rd()
s.sendall(frame(b"m1")); time.sleep(0.3); e1 = rd()
s.sendall(frame(b"boom")); time.sleep(0.3)
s.sendall(frame(b"m3")); time.sleep(0.3); e3 = rd()
m = os.urandom(4); s.sendall(bytes([0x88, 0x80]) + m); time.sleep(0.3)   # close frame
s.close()
print(notify + " broadcast=" + ("yes" if b"hello" in got else "no")
      + " echo=" + ("yes" if b"echo:m1:r7:t-r7" in e1 and b"echo:m3:r7:t-r7" in e3 else "no"))
PY
fail=0
for mode in "LOOK_X=1" "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    c="$(python3 "$TMP/client.py" "$PORT" 2>&1 | tail -1)"
    sse="$(curl -s -m 1 -N -H 'Accept: text/event-stream' "localhost:$PORT/sse" 2>/dev/null | tr -d '\r\n')"
    sleep 0.6
    r="$(curl -s -m 3 "localhost:$PORT/r")"
    placed="$(grep -c 'runs on the interpreter\|VM BUG\|interpreter fallback' "$TMP/log.txt")"
    vmroutes="$(grep -o 'VM routes: [0-9]* registered' "$TMP/log.txt")"
    errlog="$(grep -c 'message handler error: boom in ws' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$mode" != "LOOK_BYTECODE=0" ]; then
        if [ "$placed" = "0" ] && [ "$vmroutes" = "VM routes: 4 registered" ]; then echo "  OK   [$mode] all 4 routes on the VM, none placed on the tree-walk engine"
        else echo "  FAIL [$mode] routes: '$vmroutes', interpreter/fallback log lines: $placed"; fail=1; fi
    fi
    if [ "$c" = '{"ws":1,"sse":0} broadcast=yes echo=yes' ]; then echo "  OK   [$mode] broadcast reached the connection; the callback got the path parameter and the captured value"
    else echo "  FAIL [$mode] client: $c"; fail=1; fi
    if [ "$r" = '{"seen":3,"last":"m3","ws_closed":"r7","sse_closed":"yes"}' ] && [ "$errlog" = "1" ]; then echo "  OK   [$mode] three messages handled once each, the failing one logged; both close callbacks ran"
    else echo "  FAIL [$mode] r=$r, logged handler errors: $errlog"; fail=1; fi
    case "$sse" in *"data: x"*) echo "  OK   [$mode] the SSE route sent its event";; *) echo "  FAIL [$mode] SSE output: [$sse]"; fail=1;; esac
done

# A connection handler and its callbacks are dispatched from another stack frame than an HTTP
# request. The per-thread table of request::/response:: functions is bound to the request
# context it was built for; it must be rebuilt when that context is another one. It was not:
# with one worker, the ordinary request after an SSE and a WS connection wrote its response
# into a dead context and the server crashed.
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
curl -s -m 1 -N -H 'Accept: text/event-stream' "localhost:$PORT/sse" >/dev/null 2>&1
python3 "$TMP/client.py" "$PORT" >/dev/null 2>&1; sleep 0.3
ok=0; for i in 1 2 3 4; do [ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/r")" = "200" ] && ok=$((ok+1)); done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
if [ "$ok" = "4" ]; then echo "  OK   one worker: ordinary requests after an SSE and a WS connection are answered"
else echo "  FAIL one worker: $ok of 4 requests answered after an SSE and a WS connection"; fail=1; fi
[ $fail = 0 ] && echo "PASS: WebSocket and SSE run on the VM" || { echo "FAIL: ws/sse on the VM"; exit 1; }

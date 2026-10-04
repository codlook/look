#!/usr/bin/env bash
# Request isolation for handlers that outlive a request: a timer armed by a route, a
# WebSocket message handler, an SSE handler. They run on an interpreter copy derived from
# the request's copy; that second-level copy forgot where the setup globals live, so
# functions called from the handler read and WROTE the real setup values — a const or a
# top-level array changed by a WebSocket message changed for every later HTTP request.
# Each handler records that it ran (cache::) and writes into a const and a top-level array;
# a later HTTP request must still read the declared values.
# Usage: realtime_isolation_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7717}"
cat > "$TMP/app.lk" <<'LK'
use json
use cache
const { LIST = ["c0"] }
$G = ["g0"]
function w($who) { cache::set("ran_" . $who, "yes", 60); $l = LIST; $l[0] = $who; $g = $G; $g[0] = $who; return 1 }
function ran($who) { return cache::get("ran_" . $who) ?? "no" }
function arm() { timer::after(50, fn() => w("TIMER")); return 1 }
route("GET", "/arm", fn() => response::json(arm()))
route("GET", "/r", fn() => response::json(["const" => LIST[0], "global" => $G[0],
      "timer" => ran("TIMER"), "sse" => ran("SSE"), "ws_open" => ran("WSOPEN"), "ws_msg" => ran("WSMSG")]))
route("SSE", "/sse", function($sse) { w("SSE"); sse::send($sse, "x"); sse::close($sse) })
route("WS", "/ws", function($ws) { w("WSOPEN"); ws::on($ws, "message", function($d) { w("WSMSG") }) })
LK
WANT='{"const":"c0","global":"g0","timer":"yes","sse":"yes","ws_open":"yes","ws_msg":"yes"}'
fail=0
for mode in "LOOK_BYTECODE=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    curl -s -m 3 "localhost:$PORT/arm" >/dev/null; sleep 0.4
    curl -s -m 2 -N -H "Accept: text/event-stream" "localhost:$PORT/sse" >/dev/null 2>&1; sleep 0.2
    PORT="$PORT" python3 - <<'PY'
import socket, base64, os, time
s = socket.create_connection(("127.0.0.1", int(os.environ["PORT"]))); k = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % k).encode())
time.sleep(0.3); s.recv(4096)
m = os.urandom(4); p = b"hi"; s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))
time.sleep(0.5); s.close()
PY
    sleep 0.3
    got="$(curl -s -m 3 "localhost:$PORT/r")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = "$WANT" ]; then echo "  OK   [$mode] all four handlers ran; the next request reads the declared values"
    else echo "  FAIL [$mode] $got"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: timer, SSE and WebSocket handlers are isolated from later requests" || { echo "FAIL: realtime isolation"; exit 1; }

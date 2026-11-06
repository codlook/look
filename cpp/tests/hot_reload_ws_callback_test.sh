#!/usr/bin/env bash
# A WebSocket opened before a hot reload keeps running its callbacks on the program it was opened
# with; connections opened after the reload get the new program.
#
# A ws::on callback is bytecode of the program that registered it, but each event ran it with the
# CURRENT generation's setup values and builtins. After a reload the callback's global slots
# pointed into the new program's globals — another variable, or none. Now a connection keeps its
# generation (program, setup values) until it closes; the old generation is freed with its last
# connection.
# Usage: hot_reload_ws_callback_test.sh <lk-fcgi>   (needs python3)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill $pid 2>/dev/null' EXIT
PORT="${PORT:-7756}"
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
cat > "$TMP/app.lk" <<'LK'
$greeting = "hello-v1"
function tag($x) { return "v1:" . $x }
route("WS", "/ws", function($ws) {
    ws::on($ws, "message", function($m) use ($ws) { ws::send($ws, $greeting . " " . tag($m)) })
})
route("GET", "/v", fn() => response::text("v1"))
LK
write_v2() {
    { for i in $(seq 1 40); do echo "\$pad$i = \"pad-$i\""; done
      echo '$greeting = "hello-v2"'
      echo 'function tag($x) { return "v2:" . $x }'
      echo 'route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) use ($ws) { ws::send($ws, $greeting . " " . tag($m)) }) })'
      echo 'route("GET", "/v", fn() => response::text("v2"))'; } > "$TMP/app.lk.new"
    touch -d "+5 seconds" "$TMP/app.lk.new"; mv "$TMP/app.lk.new" "$TMP/app.lk"
}
cat > "$TMP/c.py" <<'PY'
import socket, base64, os, struct, sys, time
port = int(sys.argv[1])
def opn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5)
    s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(4096)
    return [s, buf.split(b"\r\n\r\n", 1)[1]]
def say(c, text):
    s = c[0]; p = text.encode(); m = os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))
    buf = c[1]
    try:
        while len(buf) < 2 or len(buf) < 2 + (buf[1] & 0x7f): buf += s.recv(4096)
    except socket.timeout: return "timeout"
    n = buf[1] & 0x7f; c[1] = buf[2 + n:]; return buf[2:2 + n].decode()
a = opn(); print("A1 " + say(a, "x"))
sys.stdout.flush(); open(sys.argv[2], "w").close()          # betiğe: şimdi yeniden yükle
while os.path.exists(sys.argv[2]): time.sleep(0.05)
print("A2 " + say(a, "y"))
b = opn(); print("B1 " + say(b, "z"))
PY
fail=0
for mode in "" "LOOK_FIBER_DISPATCH=0"; do
    m="${mode:-fibers}"
    ( cd "$TMP" && exec env $mode LOOK_HOT_RELOAD_MS=0 "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 100); do [ "$(curl -s -m 1 "localhost:$PORT/v")" = v1 ] && break; sleep 0.1; done
    python3 "$TMP/c.py" "$PORT" "$TMP/go" > "$TMP/out.txt" 2>&1 & c=$!
    for i in $(seq 1 100); do [ -e "$TMP/go" ] && break; sleep 0.05; done
    write_v2
    for i in $(seq 1 100); do [ "$(curl -s -m 1 "localhost:$PORT/v")" = v2 ] && break; sleep 0.1; done
    rm -f "$TMP/go"; wait $c
    out="$(cat "$TMP/out.txt" | tr '\n' '|')"
    [ "$out" = "A1 hello-v1 v1:x|A2 hello-v1 v1:y|B1 hello-v2 v2:z|" ] \
        && echo "  OK   [$m] the open connection kept its program, a new one got the reloaded one" \
        || { echo "  FAIL [$m] got: $out (want A1 hello-v1 v1:x|A2 hello-v1 v1:y|B1 hello-v2 v2:z|)"; fail=1; }
    kill -0 "$pid" 2>/dev/null || { echo "  FAIL [$m] the server died"; fail=1; }
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    cat > "$TMP/app.lk" <<'LK'
$greeting = "hello-v1"
function tag($x) { return "v1:" . $x }
route("WS", "/ws", function($ws) {
    ws::on($ws, "message", function($m) use ($ws) { ws::send($ws, $greeting . " " . tag($m)) })
})
route("GET", "/v", fn() => response::text("v1"))
LK
done
[ $fail = 0 ] && echo "PASS: WebSocket callbacks run on the program their connection was opened with" \
             || { echo "FAIL: WebSocket callbacks after a hot reload"; exit 1; }

#!/usr/bin/env bash
# http::stream carries state explicitly. A LOOK 2 closure captures the value and cannot change
# it, so a streaming callback cannot keep "the half line from the last chunk" in a captured
# variable any more. Instead: the first state is $opts["state"], the callback receives
# ($chunk, $state) and what it returns is the next state; the last one comes back in the
# response as "state".
# The upstream below cuts its lines in the middle of a chunk on purpose: reassembling them
# needs state that survives from one callback call to the next.
# Usage: http_stream_state_test.sh <lk>
LK="$(cd "$(dirname "${1:?usage: $0 <lk>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${srv:-}" ] && kill "$srv" 2>/dev/null' EXIT
PORT="${PORT:-7741}"
cat > "$TMP/up.py" <<'PY'
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
PIECES = [b"data: al", b"pha\ndata: be", b"ta\n", b"data: gam", b"ma\ndata: delta\n"]
while True:
    c, _ = s.accept()
    try:                                  # the readiness probe connects and leaves
        if not c.recv(65536): raise OSError
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
        for p in PIECES:
            c.sendall(("%x\r\n" % len(p)).encode() + p + b"\r\n"); time.sleep(0.05)
        c.sendall(b"0\r\n\r\n")
    except OSError:
        pass
    c.close()
PY
python3 "$TMP/up.py" "$PORT" & srv=$!
for i in $(seq 1 40); do (echo > "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break; sleep 0.1; done

cat > "$TMP/state.lk" <<LK
use http
use json
use string
\$r = http::stream("GET", "http://127.0.0.1:$PORT/", "", [], function(\$chunk, \$st) {
    \$parts = string::split(\$st["buf"] . \$chunk, "\n")
    \$n = count(\$parts)
    \$lines = \$st["lines"]
    for (\$i = 0; \$i < \$n - 1; \$i = \$i + 1) { push(\$lines, \$parts[\$i]) }
    return ["buf" => \$parts[\$n - 1], "lines" => \$lines, "calls" => \$st["calls"] + 1]
}, ["timeout" => 5000, "state" => ["buf" => "", "lines" => [], "calls" => 0]])
print(\$r["status"] . " " . json::encode(\$r["state"]["lines"]) . " rest=[" . \$r["state"]["buf"] . "] many=" . ((\$r["state"]["calls"] > 1) ? "yes" : "no"))
LK
cat > "$TMP/plain.lk" <<LK
use http
\$r = http::stream("GET", "http://127.0.0.1:$PORT/", "", [], function(\$chunk, \$state) { return \$state }, ["timeout" => 5000])
print(\$r["status"] . " state=" . ((\$r["state"] == null) ? "null" : "set"))
LK
fail=0
WANT='200 ["data: alpha","data: beta","data: gamma","data: delta"] rest=[] many=yes'
for env in "LOOK_VM_STRICT=1" "LOOK_CLI_VM=0"; do
    got="$(env LOOK_ALLOW_SSRF=1 $env "$LK" "$TMP/state.lk" 2>&1 | grep -v 'INFO' | tail -1)"
    if [ "$got" = "$WANT" ]; then echo "  OK   [$env] lines cut across chunks are reassembled through the state"
    else echo "  FAIL [$env] got: $got"; fail=1; fi
    got="$(env LOOK_ALLOW_SSRF=1 $env "$LK" "$TMP/plain.lk" 2>&1 | grep -v 'INFO' | tail -1)"
    if [ "$got" = "200 state=null" ]; then echo "  OK   [$env] without an initial state it starts as null"
    else echo "  FAIL [$env] no initial state: $got"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: http::stream carries state between callback calls" || { echo "FAIL: http::stream state"; exit 1; }

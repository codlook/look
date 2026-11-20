#!/usr/bin/env bash
# http:: reads a chunked response as strictly as the server reads a chunked request: the chunk size is
# hex digits (with an optional ";ext" and spaces before it), chunk data is followed by CRLF, and the body
# ends with the last chunk. Anything else is an error in $r["error"]; it used to give a shorter body
# silently ("1g" was read as 1, "-1" as a huge size, a cut body as complete) — a response framed one way
# by LOOK and another way by the next hop when LOOK forwards it.
# Both the buffered reader (http::get) and the streaming one (http::stream) are checked.
# Usage: http_client_chunked_test.sh <lk>
set -u
LK="$(cd "$(dirname "${1:?usage: http_client_chunked_test.sh <lk>}")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
PORT=7569

# A raw server: GET /<case> answers with the chunked body of that case, then closes the connection.
cat > "$T/srv.py" <<'PY'
import socket, sys, threading
CASES = {
    "ok":       b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
    "ext":      b"5 ;name=x\r\nhello\r\n0\r\n\r\n",
    "zeros":    b"00000000000000005\r\nhello\r\n0\r\n\r\n",
    "partial":  b"1g\r\nhello\r\n0\r\n\r\n",
    "negative": b"-1\r\nhello\r\n0\r\n\r\n",
    "huge":     b"1000000000000000\r\nhello\r\n0\r\n\r\n",
    "nocrlf":   b"5\r\nhelloXX0\r\n\r\n",
    "cutdata":  b"5\r\nhel",
    "nolast":   b"5\r\nhello\r\n",
}
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(16)
def serve(c):
    req = b""
    while b"\r\n\r\n" not in req:
        d = c.recv(4096)
        if not d: break
        req += d
    name = req.split(b" ")[1].decode().strip("/")
    c.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + CASES[name])
    c.close()
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
python3 "$T/srv.py" $PORT & SRV=$!
sleep 0.5

cat > "$T/c.lk" <<LK
use http
\$out = ""
foreach (["ok", "ext", "zeros", "partial", "negative", "huge", "nocrlf", "cutdata", "nolast"] as \$c) {
    \$r = http::get("http://127.0.0.1:$PORT/" . \$c)
    \$e = \$r["error"] ?? ""
    \$s = http::stream("GET", "http://127.0.0.1:$PORT/" . \$c, "", [], function(\$chunk, \$st) {
        return \$st . \$chunk
    }, "")
    \$se = \$s["error"] ?? ""
    print(\$c . " get=" . (\$e == "" ? "body:" . \$r["body"] : "error") . " stream=" . (\$se == "" ? "ok" : "error"))
}
LK
out="$(LOOK_ALLOW_SSRF=1 "$LK" "$T/c.lk" 2>&1 | grep -v '^\[20')"
want="ok get=body:hello world stream=ok
ext get=body:hello stream=ok
zeros get=body:hello stream=ok
partial get=error stream=error
negative get=error stream=error
huge get=error stream=error
nocrlf get=error stream=error
cutdata get=error stream=error
nolast get=error stream=error"
if [ "$out" = "$want" ]; then
    echo "PASS: http:: reads chunked responses strictly (9 cases, http::get and http::stream)"
else
    echo "FAIL: chunked responses"; diff <(echo "$want") <(echo "$out") | sed 's/^/    /'; exit 1
fi

#!/usr/bin/env bash
# http:: sends the whole request body, or reports an error.
#
# The client sent the request with a single send(). The socket has a send timeout (the
# request's "timeout" option): when the server reads slowly, send() returns after that time
# with only part of the body written, and the rest was never sent — with no error. Measured:
# an 8 MB POST to a server that starts reading after 5 s, timeout 4 s: 2.6 MB arrived and
# http::post returned error=null (the server here answers 400 for a short body; one that
# accepts what it got would have stored a truncated body).
# Usage: http_partial_send_test.sh <lk>   (needs python3)
set -u
LK="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"   # aşağıda cd yapılıyor: mutlak yol
PORT=7481
T=$(mktemp -d); trap '{ kill $sp; wait; } 2>/dev/null; rm -rf "$T"' EXIT
sp=""
cat > "$T/slow.py" <<'PY'
import socket, sys, time, threading
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
def h(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        b = c.recv(4096)
        if not b: return
        buf += b
    hdr, body = buf.split(b"\r\n\r\n", 1)
    cl = int([l.split(b":")[1] for l in hdr.split(b"\r\n") if l.lower().startswith(b"content-length")][0])
    time.sleep(5)                                   # slow reader
    c.settimeout(1.0); got = len(body)
    try:
        while got < cl:
            b = c.recv(65536)
            if not b: break
            got += len(b)
    except socket.timeout: pass
    st = b"200 OK" if got == cl else b"400 Bad Request"
    out = ("got %d of %d" % (got, cl)).encode()
    c.sendall(b"HTTP/1.1 " + st + b"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(out) + out)
    c.close()
while True:
    c, _ = s.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
PY
python3 "$T/slow.py" $PORT & sp=$!
sleep 0.5
cat > "$T/c.lk" <<LK
use http;
use string;
\$r = http::post("http://127.0.0.1:$PORT/up", string::repeat("a", 8388608), [], ["timeout" => 4000])
print("status=" . \$r["status"] . " error=" . \$r["error"] . " body=" . \$r["body"])
LK
out=$(cd "$T" && LOOK_ALLOW_SSRF=1 timeout 60 "$LK" c.lk 2>&1)
echo "  $out"
case "$out" in
    "status=200 error=null body=got 8388608 of 8388608")
        echo "PASS: the whole 8 MB body reached a slow server" ;;
    *"error=null"*)
        echo "FAIL: http:: reported no error but the server did not get the whole body"; exit 1 ;;
    *)
        echo "FAIL: unexpected result (want the whole body delivered)"; exit 1 ;;
esac

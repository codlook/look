#!/usr/bin/env bash
# HTTP/1.1 framing on a kept-alive connection: pipelined requests are all answered, in order; a
# chunked body whose trailer never ends is refused; a large header sent in small pieces does not
# cost time that grows with the square of its size.
#
#   - Every request started with a fresh buffer, so bytes of the NEXT request that arrived in the
#     same read (pipelining: the client writes request 2 before reading response 1) were thrown
#     away; the client then waited for an answer until the idle timeout.
#   - A chunked body ending "0\r\n" and then end of stream (no closing empty line) was accepted
#     as complete; it is an incomplete request (400).
#   - The end of the header block was searched from the start of the buffer after every read: a
#     2 MB header sent 64 bytes at a time cost about 3 CPU seconds. Only the new bytes are searched.
# Usage: http_pipelining_test.sh <lk-fcgi>   (needs python3)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
PORT="${PORT:-7768}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("GET", "/a", fn() => response::text("A"))
route("GET", "/b", fn() => response::text("B"))
route("POST", "/echo", fn() => response::text("E:" . request::body()))
LK
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/a" && break; sleep 0.1; done

python3 - "$PORT" "$pid" <<'PY'   # alt kabuk exec'lediği için $pid sunucunun kendisi
import socket, sys, time, re
port = int(sys.argv[1]); fail = 0
def ok(m): print("  OK   " + m)
def bad(m):
    global fail; fail = 1; print("  FAIL " + m)
def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(3)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); return s
def read_responses(s, n):
    data = b""; bodies = []
    try:
        while len(bodies) < n:
            m = re.search(rb"\r\n\r\n", data)
            if m:
                head = data[:m.start()].decode("latin1"); cl = int(re.search(r"(?i)content-length: *(\d+)", head).group(1))
                if len(data) >= m.end() + cl:
                    code = head.split(" ")[1]; bodies.append(code + " " + data[m.end():m.end() + cl].decode())
                    data = data[m.end() + cl:]; continue
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
    except socket.timeout: pass
    return bodies

s = conn()
s.sendall(b"GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 3); s.close()
(ok if r == ["200 A", "200 B", "200 A"] else bad)("three pipelined GETs in one send are all answered in order: %s" % r)

s = conn()
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /b HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 2); s.close()
(ok if r == ["200 E:hello", "200 B"] else bad)("a request after a Content-Length body in the same send: %s" % r)

s = conn()
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 2); s.close()
(ok if r == ["200 E:abc", "200 A"] else bad)("a request after a chunked body in the same send: %s" % r)

s = conn()   # gövde ayrı gönderimde gelir, sonu sonraki istekle aynı parçada
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n"); time.sleep(0.2)
s.sendall(b"abcdGET /b HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 2); s.close()
(ok if r == ["200 E:abcd", "200 B"] else bad)("a body read later and the next request in one piece: %s" % r)

s = conn()
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n"); s.shutdown(socket.SHUT_WR)
r = read_responses(s, 1); s.close()
(ok if r and r[0].startswith("400") else bad)("a chunked body whose trailer never ends is refused: %s" % r)

# 1000 KB başlık (1 MB sınırının hemen altı — üstü 431 ile kesilir) küçük, aralıklı parçalarla (her parça ayrı recv): SUNUCUNUN CPU süresi doğrusal olmalı
def cpu(pid):
    f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / float(__import__("os").sysconf("SC_CLK_TCK"))
spid = int(sys.argv[2]); c0 = cpu(spid)
s = conn()
s.sendall(b"GET /a HTTP/1.1\r\nHost: x\r\n")
piece = b"a:\r\n" * 32   # kısa başlık satırları: her 4 baytta bir \r → tarama memchr ile atlayamaz
for i in range(1000 * 1024 // 128):
    s.send(piece)
    time.sleep(0.0001)
s.sendall(b"\r\n")
r = read_responses(s, 1); s.close(); dc = cpu(spid) - c0
# Yanıt 431 (500 alan sınırı) — ölçülen, tüm başlığı alana dek sunucunun harcadığı CPU. Doğrusal ~0.1 s;
# eski karesel tarama bu boyutta ~0.75 s (2 MB'ta ~3 s) → eşik 0.4 eski hatayı hâlâ yakalar.
(ok if dc < 0.4 else bad)("a 1000 KB header of short lines in small pieces costs the server %.2f CPU s (%s)" % (dc, r))
sys.exit(fail)
PY
rc=$?
kill -0 "$pid" 2>/dev/null || { echo "  FAIL the server died"; rc=1; }
[ $rc = 0 ] && echo "PASS: pipelined requests are answered; incomplete chunked trailers refused; header scan is linear" \
           || { echo "FAIL: HTTP/1.1 framing on a kept-alive connection"; exit 1; }

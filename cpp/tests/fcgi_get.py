#!/usr/bin/env python3
# Minimal FastCGI client for tests: fcgi_get.py PORT URI SCRIPT_FILENAME -> prints the body
import socket, struct, sys, os
def rec(t, rid, content):
    return struct.pack("!BBHHBx", 1, t, rid, len(content), 0) + content
def nv(k, v):
    k = k.encode(); v = v.encode()
    def L(n): return bytes([n]) if n < 128 else struct.pack("!I", n | 0x80000000)
    return L(len(k)) + L(len(v)) + k + v
port = int(sys.argv[1]); uri = sys.argv[2]; script = sys.argv[3]
path, _, qs = uri.partition("?")
s = socket.create_connection(("127.0.0.1", port), timeout=10)
p = b"".join(nv(k, v) for k, v in [("REQUEST_METHOD", "GET"), ("REQUEST_URI", uri), ("SCRIPT_NAME", path),
     ("PATH_INFO", path), ("QUERY_STRING", qs), ("SERVER_PROTOCOL", "HTTP/1.1"), ("HTTP_HOST", "x"),
     ("REMOTE_ADDR", "127.0.0.1"), ("SCRIPT_FILENAME", script), ("DOCUMENT_ROOT", os.path.dirname(script)),
     ("SERVER_NAME", "x"), ("SERVER_PORT", "80")])
s.sendall(rec(1, 1, struct.pack("!HB5x", 1, 0)) + rec(4, 1, p) + rec(4, 1, b"") + rec(5, 1, b""))
out = b""
try:
    while True:
        h = s.recv(8, socket.MSG_WAITALL)
        if len(h) < 8: break
        v, t, rid, cl, pl = struct.unpack("!BBHHB", h[:7])
        body = s.recv(cl + pl, socket.MSG_WAITALL)[:cl] if cl + pl else b""
        if t == 6: out += body
        if t == 3: break
except Exception as e:
    print("[client error %r]" % e)
print(out.decode("utf-8", "replace").split("\r\n\r\n", 1)[-1])

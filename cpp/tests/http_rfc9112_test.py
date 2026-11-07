#!/usr/bin/env python3
# HTTP/1.1 request parsing boundary table (RFC 9112), with Go net/http as the reference.
# Every row is a raw request and the status line(s) LOOK must answer with. Rows marked "go" are
# what Go 1.27's net/http answers too; rows marked "strict" are request-smuggling defences where
# LOOK stays stricter than Go (bare LF, obs-fold, CL+TE, loose chunk lines) and answers 400.
# A rejected request now gets its status line before the connection closes (it used to be closed
# with no answer at all).
#
# Usage: http_rfc9112_test.py <look_port> [go_port]
#   With go_port (tests/http_go_oracle, `go run . <port>`), the "go" rows are also sent to Go and
#   any row where Go answers differently is reported — the table is re-checked against Go.
import re, socket, sys, time

def send(port, data, linger=0.0):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=4)
        s.sendall(data)
        if linger: time.sleep(linger)
        try: s.shutdown(socket.SHUT_WR)
        except OSError: pass
        out = b""
        try:
            while True:
                c = s.recv(65536)
                if not c: break
                out += c
        except OSError: pass
        s.close()
    except OSError as e:
        return "ERR " + str(e), b""
    codes = [c.decode() for c in re.findall(rb"HTTP/1\.[01] (\d{3}) ", out)]
    return ("|".join(codes) if codes else "CLOSED"), out

H = b"Host: x\r\n"
G, P = b"GET / HTTP/1.1\r\n", b"POST / HTTP/1.1\r\n"
TE = b"Transfer-Encoding: chunked\r\n"
def hdrs(n): return b"".join(b"X-%d: v\r\n" % i for i in range(n))

# (name, request, expected, kind)   kind: go = Go answers the same; strict = LOOK stricter than Go
ROWS = [
    ("valid GET",                 G + H + b"\r\n",                                        "200", "go"),
    ("HTTP/1.0 without Host",     b"GET / HTTP/1.0\r\n\r\n",                              "200", "go"),
    ("HTTP/1.1 without Host",     G + b"\r\n",                                            "400", "go"),
    ("two Host (same value)",     G + H + H + b"\r\n",                                    "400", "go"),
    ("two Host (different)",      G + H + b"Host: y\r\n\r\n",                             "400", "go"),
    ("empty Host",                G + b"Host:\r\n\r\n",                                   "200", "go"),
    ("Host with a space",         G + b"Host: a b\r\n\r\n",                               "400", "go"),
    ("Host with '/'",             G + b"Host: a/b\r\n\r\n",                               "400", "go"),
    ("Host with userinfo",        G + b"Host: u@x\r\n\r\n",                               "400", "go"),
    ("Host IPv6 literal",         G + b"Host: [::1]:80\r\n\r\n",                          "200", "go"),
    ("space inside a field name", G + H + b"X Y: 1\r\n\r\n",                              "400", "go"),
    ("empty field name",          G + H + b": 1\r\n\r\n",                                 "400", "go"),
    ("line without ':'",          G + H + b"garbage\r\n\r\n",                             "400", "go"),
    ("'@' in a field name",       G + H + b"X@Y: 1\r\n\r\n",                              "400", "go"),
    ("NUL in a value",            G + H + b"X: a\x00b\r\n\r\n",                           "400", "go"),
    ("DEL in a value",            G + H + b"X: a\x7fb\r\n\r\n",                           "400", "go"),
    ("HTAB in a value",           G + H + b"X: a\tb\r\n\r\n",                             "200", "go"),
    ("UTF-8 in a value",          G + H + "X: çğ\r\n\r\n".encode(),                       "200", "go"),
    ("500 header fields",         G + H + hdrs(499) + b"\r\n",                            "200", "go"),
    ("501 header fields",         G + H + hdrs(500) + b"\r\n",                            "431", "go"),
    ("10000 header fields",       G + H + hdrs(10000) + b"\r\n",                          "431", "go"),
    ("900 KB header block",       G + H + b"X: " + b"a" * 900000 + b"\r\n\r\n",           "200", "go"),
    ("1.5 MB header block",       G + H + b"X: " + b"a" * 1500000 + b"\r\n\r\n",          "431", "go"),
    ("version HTTP/9",            b"GET / HTTP/9\r\n" + H + b"\r\n",                      "400", "go"),
    ("version HTTP/2.0",          b"GET / HTTP/2.0\r\n" + H + b"\r\n",                    "505", "go"),
    ("version HTTP/0.9",          b"GET / HTTP/0.9\r\n" + H + b"\r\n",                    "505", "go"),
    ("version HTTP/1.2",          b"GET / HTTP/1.2\r\n" + H + b"\r\n",                    "200", "go"),
    ("version HTTP/1.10",         b"GET / HTTP/1.10\r\n" + H + b"\r\n",                   "400", "go"),
    ("version lowercase",         b"GET / http/1.1\r\n" + H + b"\r\n",                    "400", "go"),
    ("'@' in the method",         b"G@T / HTTP/1.1\r\n" + H + b"\r\n",                    "400", "go"),
    ("two spaces after method",   b"GET  / HTTP/1.1\r\n" + H + b"\r\n",                   "400", "go"),
    ("space inside the target",   b"GET /a b HTTP/1.1\r\n" + H + b"\r\n",                 "400", "go"),
    ("control byte in target",    b"GET /a\x01b HTTP/1.1\r\n" + H + b"\r\n",              "400", "go"),
    ("target without '/'",        b"GET abc HTTP/1.1\r\n" + H + b"\r\n",                  "400", "go"),
    ("absolute-form",             b"GET http://y/ HTTP/1.1\r\n" + H + b"\r\n",            "200", "go"),
    ("absolute-form, query only", b"GET http://y?q=1 HTTP/1.1\r\n" + H + b"\r\n",         "200", "go"),
    ("absolute-form, no Host",    b"GET http://y/ HTTP/1.1\r\n\r\n",                      "400", "go"),
    ("OPTIONS *",                 b"OPTIONS * HTTP/1.1\r\n" + H + b"\r\n",                "200", "go"),
    ("leading CRLF",              b"\r\n" + G + H + b"\r\n",                              "400", "go"),
    ("space before ':'",          G + H + b"X : a\r\n\r\n",                               "400", "go"),
    ("two different CL",          P + H + b"Content-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", "400", "go"),
    ("two equal CL",              P + H + b"Content-Length: 3\r\nContent-Length: 3\r\n\r\nabc",  "200", "go"),
    ("CL +3",                     P + H + b"Content-Length: +3\r\n\r\nabc",               "400", "go"),
    ("CL 3,3",                    P + H + b"Content-Length: 3,3\r\n\r\nabc",              "400", "go"),
    ("TE gzip",                   P + H + b"Transfer-Encoding: gzip\r\n\r\n",             "501", "go"),
    ("TE chunked, gzip",          P + H + b"Transfer-Encoding: chunked, gzip\r\n\r\n",    "501", "go"),
    ("TE identity",               P + H + b"Transfer-Encoding: identity\r\n\r\n",         "501", "go"),
    ("TE Chunked (case)",         P + H + b"Transfer-Encoding: Chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", "200", "go"),
    ("chunk extension",           P + H + TE + b"\r\n3;a=b\r\nabc\r\n0\r\n\r\n",          "200", "go"),
    ("Expect: foo",               P + H + b"Expect: foo\r\nContent-Length: 3\r\n\r\nabc", "417", "go"),
    ("two pipelined",             G + H + b"\r\n" + G + H + b"\r\n",                      "200|200", "go"),
    ("pipelined, bad second",     G + H + b"\r\nGARBAGE\r\n\r\n",                         "200|400", "go"),
    # Smuggling defences: Go accepts these, LOOK refuses — a front proxy may frame them differently.
    ("bare LF line end",          b"GET / HTTP/1.1\n" + H + b"\r\n",                      "400", "strict"),
    ("obs-fold",                  G + H + b"X: a\r\n b\r\n\r\n",                          "400", "strict"),
    ("CL and TE together",        P + H + b"Content-Length: 3\r\n" + TE + b"\r\n3\r\nabc\r\n0\r\n\r\n", "400", "strict"),
    ("bare LF in chunk ext",      P + H + TE + b"\r\n3;a\nb\r\nabc\r\n0\r\n\r\n",         "400", "strict"),
    ("bare LF after chunk size",  P + H + TE + b"\r\n3\nabc\r\n0\r\n\r\n",                "400", "strict"),
    ("bare LF after chunk data",  P + H + TE + b"\r\n3\r\nabc\n0\r\n\r\n",                "400", "strict"),
    ("chunk size 0x3",            P + H + TE + b"\r\n0x3\r\nabc\r\n0\r\n\r\n",            "400", "strict"),
    ("chunk size with space",     P + H + TE + b"\r\n3 \r\nabc\r\n0\r\n\r\n",             "400", "strict"),
    ("trailer with bare LF",      P + H + TE + b"\r\n3\r\nabc\r\n0\r\nX: 1\n\r\n",        "400", "strict"),
    ("trailer without ':'",       P + H + TE + b"\r\n3\r\nabc\r\n0\r\nbad\r\n\r\n",       "400", "strict"),
    ("GET *",                     b"GET * HTTP/1.1\r\n" + H + b"\r\n",                    "400", "strict"),
]

look = int(sys.argv[1]); gop = int(sys.argv[2]) if len(sys.argv) > 2 else 0
fail = 0
w = max(len(r[0]) for r in ROWS)
for name, req, exp, kind in ROWS:
    got, _ = send(look, req)
    ok = got == exp
    line = "  %-*s  want %-8s got %-8s %s" % (w, name, exp, got, "PASS" if ok else "FAIL")
    if gop and kind == "go":
        g, _ = send(gop, req)
        g = g.replace("100|", "")          # Go answers "100 Continue" first when it reads a body
        if g != exp: line += "   (Go answers %s — table out of date)" % g; fail = 1
    print(line)
    if not ok: fail = 1

# Host from the absolute-form target replaces the Host header (RFC 9112 §3.2.2, Go r.Host).
got, out = send(look, b"GET http://real.example:8080/host HTTP/1.1\r\nHost: other\r\n\r\n")
ok = b"H[real.example:8080]" in out
print("  %-*s  %s" % (w, "absolute-form sets Host", "PASS" if ok else "FAIL " + repr(out[-60:])))
fail |= not ok

# OPTIONS * keeps the connection: a second request on it is answered.
got, _ = send(look, b"OPTIONS * HTTP/1.1\r\n" + H + b"\r\n" + G + H + b"Connection: close\r\n\r\n")
ok = got == "200|200"
print("  %-*s  %s" % (w, "OPTIONS * then GET (keep-alive)", "PASS" if ok else "FAIL got " + got))
fail |= not ok

# Expect: 100-continue — the interim answer comes before the body is sent (curl waits ~1 s for it).
try:
    s = socket.create_connection(("127.0.0.1", look), timeout=4)
    s.sendall(P + H + b"Expect: 100-continue\r\nContent-Length: 3\r\n\r\n")
    s.settimeout(0.8)
    first = s.recv(100)
    s.settimeout(4)
    s.sendall(b"abc")
    rest = b""
    while b"B[abc]" not in rest:
        c = s.recv(4096)
        if not c: break
        rest += c
    s.close()
    ok = first.startswith(b"HTTP/1.1 100 Continue\r\n\r\n") and b"200" in rest and b"B[abc]" in rest
except OSError as e:
    ok, first, rest = False, b"", str(e).encode()
print("  %-*s  %s" % (w, "Expect: 100-continue", "PASS" if ok else "FAIL first=%r rest=%r" % (first[:40], rest[:60])))
fail |= not ok

print("PASS: HTTP/1.1 request parsing matches the RFC 9112 table" if not fail else "FAIL: HTTP/1.1 request parsing table")
sys.exit(1 if fail else 0)

#!/usr/bin/env python3
# HTTP request parsing differential fuzz test: random requests — valid ones, edge cases and the
# request-smuggling shapes (Content-Length with Transfer-Encoding, two Content-Lengths, obs-fold,
# bare LF/CR, malformed chunk sizes, bodies longer or shorter than announced) — are sent as raw bytes
# to `lk-fcgi --mode http` and to Go's net/http (tests/input_fuzz/http_referee.go). A second request
# (GET /sentinel) follows every request on the same connection, so how each server frames the first
# request shows in whether and where the sentinel is answered.
#
# Each difference is classified by direction:
#   LOOSER    LOOK accepts what Go rejects — a possible security bug (smuggling, desync): FAIL.
#   FRAMING   both accept but read a different request (method, body) or a different number of
#             requests on the connection — the smuggling signal itself: FAIL.
#   STRICTER  LOOK rejects what Go accepts. FAIL only for a request that is valid HTTP/1.1. Two
#             kinds do not fail and are only counted: the smuggling defences where LOOK is deliberately
#             stricter than Go (bare LF, obs-fold, Content-Length with Transfer-Encoding, loose chunk
#             lines, a body cut short, HTTP/1.0 with Transfer-Encoding), and requests that RFC 9110/9112
#             make invalid (an empty header name, a Content-Length that is not digits, POST *, ...),
#             which Go happens to accept. Each mutation marks the variants it knows are invalid.
#
# Usage: python3 tests/http_differential_test.py <lk-fcgi> [--cases N] [--seed S] [--referee BIN]
# Without --referee the referee is built with `go build` (Go must be on PATH).
# Default: 2500 requests, fixed seed. LOOK_HTTP_FUZZ_CASES=N runs more.
import argparse
import os
import random
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SENTINEL = b"GET /sentinel HTTP/1.1\r\nHost: s\r\n\r\n"
SENTINEL_ECHO = b"GET|/sentinel|0|"
METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"]

APP = "\n".join(
    ['function echo() { return response::text(request::method() . "|" . request::path() . "|" . '
     'strlen(request::body()) . "|" . request::body()) }']
    + ['route("%s", "/e", function() { return echo() })' % m for m in METHODS]
    + ['route("GET", "/sentinel", function() { return echo() })']) + "\n"

# Mutations a request can get. The tag names the kind; DESIGN marks the kinds where LOOK is stricter
# than Go on purpose (request-smuggling defences, see tests/http_rfc9112_test.py "strict" rows).
DESIGN = {"bare-lf", "bare-cr", "obs-fold", "cl+te", "chunk-loose", "body-short", "te-http10"}


# ── request model ────────────────────────────────────────────────────────────
class Req:
    def __init__(self, method, target, body, chunked):
        self.method, self.target, self.version = method, target, "HTTP/1.1"
        self.headers = [("Host", "x")]
        self.body = body
        self.chunked = chunked
        if chunked:
            self.headers.append(("Transfer-Encoding", "chunked"))
        elif body or method in ("POST", "PUT", "PATCH"):
            self.headers.append(("Content-Length", str(len(body))))
        self.eol = b"\r\n"
        self.raw_body = None      # bytes after the header block (None = built from body/chunked)
        self.extra_lines = []     # raw header lines inserted as they are
        self.tags = set()

    def chunks(self, r):
        if not self.body:
            return b"0\r\n\r\n"
        out, b = b"", self.body
        while b:
            n = r.randint(1, len(b))
            out += b"%x\r\n" % n + b[:n] + b"\r\n"
            b = b[n:]
        return out + b"0\r\n\r\n"

    def build(self, r):
        head = ("%s %s %s" % (self.method, self.target, self.version)).encode("latin-1") + self.eol
        for k, v in self.headers:
            head += k.encode("latin-1") + b": " + v.encode("latin-1") + self.eol
        for l in self.extra_lines:
            head += l
        head += self.eol
        if self.raw_body is not None:
            return head + self.raw_body
        return head + (self.chunks(r) if self.chunked else self.body)


def set_header(req, name, value):
    req.headers = [(k, v) for k, v in req.headers if k.lower() != name.lower()]
    req.headers.append((name, value))


def mutate(r, req):
    m = r.choice(MUTATIONS)
    m(r, req)


def m_cl_te(r, req):
    req.tags.add("cl+te")
    body = req.body or b"hello"
    req.body = body
    req.chunked = True
    hs = [("Transfer-Encoding", "chunked"), ("Content-Length", r.choice([str(len(body)), "0", "3", "100"]))]
    r.shuffle(hs)
    req.headers = [h for h in req.headers if h[0].lower() not in ("content-length", "transfer-encoding")] + hs


def m_dup_cl(r, req):
    req.tags.add("dup-cl")
    if req.chunked:
        return
    n = len(req.body)
    other = r.choice([n, n, n + 1, 0])
    if other != n:
        req.tags.add("invalid")         # two different lengths (RFC 9112 §6.3)
    req.headers.append(("Content-Length", str(other)))


def m_cl_format(r, req):
    req.tags.add("cl-format")
    if req.chunked:
        return
    n = len(req.body)
    v = r.choice(["+%d", " %d", "%d ", "0%d", "0x%x", "-%d", "%d,%d", "%d, %d", "", "99999999999999999999",
                  "%d.0", "%de0"])
    if v not in (" %d", "%d ", "0%d"):
        req.tags.add("invalid")         # Content-Length = 1*DIGIT (RFC 9110 §8.6)
    try:
        v = v % ((n, n) if v.count("%") == 2 else (n,)) if "%" in v else v
    except TypeError:
        pass
    set_header(req, "Content-Length", v)


def m_te_variant(r, req):
    req.tags.add("te-variant")
    req.body = req.body or b"hi"
    req.chunked = True
    v = r.choice(["Chunked", "CHUNKED", " chunked", "chunked ", "\tchunked", "chunked, chunked",
                  "identity, chunked", "gzip, chunked", "chunked, identity", "xchunked", "chunked\x0b",
                  "", "identity", "chunked;q=1"])
    req.headers = [h for h in req.headers if h[0].lower() not in ("content-length", "transfer-encoding")]
    req.headers.append(("Transfer-Encoding", v))
    if v.strip().lower() not in ("chunked",):
        req.tags.add("te-odd")


def m_dup_te(r, req):
    req.tags.add("dup-te")
    req.body = req.body or b"hi"
    req.chunked = True
    req.headers = [h for h in req.headers if h[0].lower() not in ("content-length", "transfer-encoding")]
    req.headers += [("Transfer-Encoding", "chunked"), ("Transfer-Encoding", r.choice(["chunked", "identity", "gzip"]))]


def m_te_http10(r, req):
    req.tags.add("te-http10")
    req.version = "HTTP/1.0"
    req.body = req.body or b"hi"
    req.chunked = True
    req.headers = [h for h in req.headers if h[0].lower() not in ("content-length", "transfer-encoding")]
    req.headers.append(("Transfer-Encoding", "chunked"))


def m_obs_fold(r, req):
    req.tags.add("obs-fold")
    req.extra_lines.append(b"X-A: one\r\n" + r.choice([b" two\r\n", b"\ttwo\r\n"]))


def m_bare_lf(r, req):
    req.tags.add("bare-lf")
    req.eol = b"\n"


def m_bare_cr(r, req):
    req.tags.add("bare-cr")
    req.extra_lines.append(b"X-B: v\rX-C: w\r\n")


def m_space_colon(r, req):
    req.tags.update(("space-before-colon", "invalid"))   # RFC 9112 §5.1: MUST be rejected
    req.extra_lines.append(r.choice([b"X-D : v\r\n", b"Content-Length : 0\r\n", b"Host : y\r\n"]))


def m_bad_name(r, req):
    req.tags.update(("bad-header-name", "invalid"))
    req.extra_lines.append(r.choice([b"X D: v\r\n", b"X@D: v\r\n", b": v\r\n", b"X(D): v\r\n", b"X\x01D: v\r\n"]))


def m_value_ctl(r, req):
    req.tags.add("header-value-ctl")
    line = r.choice([b"X-E: a\x00b\r\n", b"X-E: a\x7fb\r\n", b"X-E: a\x01b\r\n", b"X-E: a\tb\r\n",
                     b"X-E: \xc3\xa7\r\n", b"X-E: \xff\r\n"])
    if line[6:7] in (b"\x00", b"\x7f", b"\x01"):
        req.tags.add("invalid")         # a control character in a field value
    req.extra_lines.append(line)


def m_host(r, req):
    req.tags.add("host")
    k = r.random()
    if k < 0.3:
        req.headers = [h for h in req.headers if h[0].lower() != "host"]
        req.tags.update(("no-host", "invalid"))
    elif k < 0.55:
        req.headers.append(("Host", r.choice(["x", "y"])))
        req.tags.update(("dup-host", "invalid"))
    else:
        h = r.choice(["x y", "x/y", "[::1", "x:99999", "x:", "", "X.Example", "x:80",
                      "[::1]:8080", "a_b", "x..y", "\xc3\xa7"])
        if h in ("x y", "x/y", "[::1", "\xc3\xa7"):
            req.tags.add("invalid")     # not a uri-host [ ":" port ]
        set_header(req, "Host", h)


def m_reqline(r, req):
    req.tags.add("request-line")
    k = r.randint(0, 9)
    if k == 0:
        req.method = req.method.lower()
    elif k == 1:
        req.method = req.method + " "; req.tags.add("invalid")
    elif k == 2:
        req.version = r.choice(["HTTP/1.2", "HTTP/2.0", "HTTP/1.10", "http/1.1", "HTTP/1", "HTTP/1.0", "HTTP/9.9"])
        if req.version in ("HTTP/1.10", "http/1.1", "HTTP/1"):
            req.tags.add("invalid")
    elif k == 3:
        req.target = r.choice(["*", "http://x/e", "e", "/e#f", "/e?x=1", "/e?a=%20&b", "/%65", "//e", "/e/../e"])
        if req.target in ("e", "/e#f") or (req.target == "*" and req.method != "OPTIONS"):
            req.tags.add("invalid")
    elif k == 4:
        req.target = "/e x"; req.tags.add("invalid")
    elif k == 5:
        req.method = "FOO"
    elif k == 6:
        req.method = req.method + "\t"; req.tags.add("invalid")
    elif k == 7:
        req.version = req.version + " "; req.tags.add("invalid")
    elif k == 8:
        req.target = "/e?" + "a" * r.choice([100, 9000])
    else:
        req.method = ""; req.tags.add("invalid")


def m_chunk(r, req):
    req.tags.add("chunk")
    body = req.body or b"hello"
    req.body = body
    req.chunked = True
    req.headers = [h for h in req.headers if h[0].lower() not in ("content-length", "transfer-encoding")]
    req.headers.append(("Transfer-Encoding", "chunked"))
    n = len(body)
    k = r.randint(0, 11)
    loose = True
    if k == 0:
        raw = b"%xg\r\n" % n + body + b"\r\n0\r\n\r\n"
    elif k == 1:
        raw = b"-1\r\n" + body + b"\r\n0\r\n\r\n"
    elif k == 2:
        raw = b"0x%x\r\n" % n + body + b"\r\n0\r\n\r\n"
    elif k == 3:
        raw = b"000%x\r\n" % n + body + b"\r\n0\r\n\r\n"; loose = False
    elif k == 4:
        raw = b"%x;a=b\r\n" % n + body + b"\r\n0\r\n\r\n"; loose = False
    elif k == 5:
        raw = b"%x;a\x01\r\n" % n + body + b"\r\n0\r\n\r\n"
    elif k == 6:
        raw = b"%x\r\n" % n + body + b"XX0\r\n\r\n"
    elif k == 7:
        raw = b"%x\r\n" % n + body + b"\r\n0\r\nX-T: v\r\n\r\n"; loose = False
    elif k == 8:
        raw = b"%x\n" % n + body + b"\n0\n\n"
    elif k == 9:
        raw = b"%x \r\n" % n + body + b"\r\n0\r\n\r\n"
    elif k == 10:
        raw = b"%x\r\n" % (n + 3) + body + b"\r\n0\r\n\r\n"
    else:
        raw = b"%x\r\n" % n + body + b"\r\n0\r\n"          # missing the final CRLF
    if loose:
        req.tags.add("chunk-loose")
    req.raw_body = raw


def m_body_overrun(r, req):
    req.tags.add("body-overrun")
    if req.chunked:
        return
    extra = r.choice([b"X", b"GET / HTTP/1.1\r\n\r\n", b"\r\n"])
    if extra == b"\r\n":
        req.tags.add("crlf-before-next")
    req.raw_body = req.body + extra


def m_body_short(r, req):
    req.tags.add("body-short")
    if req.chunked:
        return
    set_header(req, "Content-Length", str(len(req.body) + r.randint(1, 5)))


def m_long_header(r, req):
    req.tags.add("long-header")
    req.headers.append(("X-Long", "v" * r.choice([1000, 8000, 20000])))


def m_many_headers(r, req):
    req.tags.add("many-headers")
    for i in range(r.choice([50, 120])):
        req.headers.append(("X-%d" % i, "v"))


MUTATIONS = [m_cl_te, m_dup_cl, m_cl_format, m_te_variant, m_dup_te, m_te_http10, m_obs_fold, m_bare_lf,
             m_bare_cr, m_space_colon, m_bad_name, m_value_ctl, m_host, m_reqline, m_chunk, m_body_overrun,
             m_body_short, m_long_header, m_many_headers]


def gen(r):
    method = r.choice(METHODS)
    body = b""
    if method in ("POST", "PUT", "PATCH") or r.random() < 0.1:
        body = bytes(r.choice(b"abcxyz019 -_=&{}\"") for _ in range(r.randint(0, 12)))
    req = Req(method, r.choice(["/e", "/e", "/e?x=1"]), body, chunked=bool(body) and r.random() < 0.35)
    for _ in range(r.choice([0, 1, 1, 1, 2, 2, 3])):
        mutate(r, req)
    return req.build(r), req.tags


# ── transport ────────────────────────────────────────────────────────────────
def exchange(port, data):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
    except OSError as e:
        return None, str(e)
    out = b""
    try:
        s.sendall(data)
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except OSError:
        pass
    finally:
        s.close()
    return out, None


def parse_responses(raw):
    """[(status, body)] for each complete response; stops at anything it cannot read."""
    res = []
    i = 0
    while i < len(raw):
        m = re.match(rb"HTTP/1\.[01] (\d{3})[^\r\n]*\r\n", raw[i:])
        if not m:
            break
        status = int(m.group(1))
        he = raw.find(b"\r\n\r\n", i)
        if he < 0:
            res.append((status, b""))
            break
        head = raw[i:he].decode("latin-1").lower()
        j = he + 4
        cl = re.search(r"\r\ncontent-length: *(\d+)", head)
        if cl:
            n = int(cl.group(1))
            body = raw[j:j + n]
            j += n
        elif "transfer-encoding: chunked" in head:
            body = b""
            while True:
                e = raw.find(b"\r\n", j)
                if e < 0:
                    break
                n = int(raw[j:e].split(b";")[0] or b"0", 16)
                j = e + 2
                if n == 0:
                    j = raw.find(b"\r\n\r\n", j - 2) + 4 if raw.find(b"\r\n\r\n", j - 2) >= 0 else len(raw)
                    break
                body += raw[j:j + n]
                j += n + 2
        else:
            body = raw[j:]
            j = len(raw)
        res.append((status, body))
        i = j
    return res


def summary(raw):
    """What a server did with request + sentinel: (first kind, first echo, sentinel answered, count)."""
    rs = parse_responses(raw or b"")
    if not rs:
        return ("closed", None, False, 0)
    st, body = rs[0]
    if st >= 400 and st not in (404, 405):
        kind = "reject"
    elif st in (404, 405):
        kind = "noroute"
    else:
        kind = "ok"
    sentinel = any(b == SENTINEL_ECHO for s, b in rs[1:])
    echo = body if kind == "ok" else None
    return (kind, echo, sentinel, len(rs))


# ── servers ──────────────────────────────────────────────────────────────────
def wait_port(port, proc, name):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            if proc.poll() is not None:
                raise SystemExit("FAIL: %s exited at start" % name)
            time.sleep(0.1)
    raise SystemExit("FAIL: %s did not start" % name)


def build_referee(tmp):
    exe = os.path.join(tmp, "http_referee")
    env = dict(os.environ, CGO_ENABLED="0", GO111MODULE="off")
    subprocess.run(["go", "build", "-o", exe, os.path.join(HERE, "input_fuzz", "http_referee.go")], check=True, env=env)
    return exe


def accepted(s):
    return s[0] in ("ok", "noroute")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fcgi")
    ap.add_argument("--cases", type=int, default=int(os.environ.get("LOOK_HTTP_FUZZ_CASES", "2500")))
    ap.add_argument("--seed", type=int, default=20261121)
    ap.add_argument("--referee")
    ap.add_argument("--look-port", type=int, default=7571)
    ap.add_argument("--go-port", type=int, default=7572)
    ap.add_argument("--show", type=int, default=2)
    a = ap.parse_args()
    r = random.Random(a.seed)
    found = {}

    def note(kind, tags, req, look, go):
        key = (kind, ",".join(sorted(tags)) or "-")
        found.setdefault(key, []).append((req, look, go))

    with tempfile.TemporaryDirectory() as tmp:
        app = os.path.join(tmp, "app.lk")
        open(app, "w").write(APP)
        exe = a.referee or build_referee(tmp)
        log = open(os.path.join(tmp, "look.log"), "w")
        lk = subprocess.Popen([os.path.abspath(a.fcgi), "--mode", "http", "--port", str(a.look_port), app],
                              stdout=log, stderr=log)
        go = subprocess.Popen([exe, str(a.go_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_port(a.look_port, lk, "lk-fcgi")
            wait_port(a.go_port, go, "the Go referee")
            for _ in range(a.cases):
                data, tags = gen(r)
                wire = data + SENTINEL
                lr, _ = exchange(a.look_port, wire)
                gr, _ = exchange(a.go_port, wire)
                ls, gs = summary(lr), summary(gr)
                if lk.poll() is not None:
                    note("CRASH", tags, data, ls, gs)
                    break
                if accepted(ls) and not accepted(gs):
                    note("LOOSER", tags, data, ls, gs)
                elif accepted(gs) and not accepted(ls):
                    if tags & DESIGN:
                        note("STRICTER-BY-DESIGN", tags, data, ls, gs)
                    elif "invalid" in tags:
                        note("STRICTER-ON-INVALID", tags, data, ls, gs)
                    else:
                        note("STRICTER", tags, data, ls, gs)
                elif ls[0] == "ok" and gs[0] == "ok":
                    l_echo = ls[1].split(b"|", 3)
                    g_echo = gs[1].split(b"|", 3)
                    if l_echo[0] != g_echo[0] or l_echo[2:] != g_echo[2:] or ls[2] != gs[2]:
                        note("FRAMING", tags, data, ls, gs)
                elif accepted(ls) and accepted(gs) and ls[2] != gs[2]:
                    note("FRAMING", tags, data, ls, gs)
        finally:
            lk.kill()
            go.kill()
            lk.wait()
            go.wait()

    print("http differential: %d requests, seed %d" % (a.cases, a.seed))
    bad = 0
    order = {"CRASH": 0, "LOOSER": 1, "FRAMING": 2, "STRICTER": 3, "STRICTER-BY-DESIGN": 4, "STRICTER-ON-INVALID": 5}
    by_design = sum(len(v) for (k, _), v in found.items() if k == "STRICTER-BY-DESIGN")
    on_invalid = sum(len(v) for (k, _), v in found.items() if k == "STRICTER-ON-INVALID")
    if by_design:
        print("stricter by design (smuggling defences, not a failure): %d requests" % by_design)
    if on_invalid:
        print("stricter on requests RFC 9110/9112 make invalid (Go accepts them; not a failure): %d requests" % on_invalid)
    for (kind, tags), items in sorted(found.items(), key=lambda kv: (order[kv[0][0]], -len(kv[1]))):
        failing = kind not in ("STRICTER-BY-DESIGN", "STRICTER-ON-INVALID")
        bad += failing
        if failing:
            print("%s (%d requests) [%s]" % (kind, len(items), tags))
            for req, look, go in sorted(items, key=lambda x: len(x[0]))[:a.show]:
                print("    request %r" % (req[:220],))
                print("      LOOK: %s %r sentinel=%s responses=%d" % (look[0], look[1] and look[1][:60], look[2], look[3]))
                print("      Go:   %s %r sentinel=%s responses=%d" % (go[0], go[1] and go[1][:60], go[2], go[3]))
    if not bad:
        print("PASS: LOOK reads every request as Go does, or stricter where that is by design")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

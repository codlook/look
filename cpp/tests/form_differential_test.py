#!/usr/bin/env python3
# URL / form decoding differential fuzz test: random query strings, form bodies and path segments
# go to a real `lk-fcgi --mode http` server, and what the program sees (request::all(),
# request::param()) is compared with a small, obviously correct model of the rules:
#   * a query string or an application/x-www-form-urlencoded body is split on '&'; an empty piece is
#     skipped; the first '=' splits name and value (no '=' -> the value is "");
#   * '+' is a space; '%' followed by exactly two hex digits is that byte; any other '%' stays as it is;
#   * a repeated name keeps its last value;
#   * request::all() holds every name once: a name in both the query string and the body has the
#     body's value (Go's Request.Form);
#   * a route parameter is decoded the same way except that '+' stays '+';
#   * the values travel back through json::encode, so invalid UTF-8 shows as U+FFFD per byte.
# Any difference fails and prints the shortest example of each kind.
#
# Usage: python3 tests/form_differential_test.py <lk-fcgi> [--cases N] [--seed S] [--port P]
# Default: 1500 requests, fixed seed. LOOK_FORM_FUZZ_CASES=N runs more.
import argparse
import http.client
import json
import os
import random
import re
import subprocess
import sys
import tempfile
import time

APP = r'''
route("GET", "/q", function() { print(json::encode(["all" => request::all()])) })
route("POST", "/q", function() { print(json::encode(["all" => request::all()])) })
route("GET", "/p/{v}", function($v) { print(json::encode(["arg" => $v, "param" => request::param("v")])) })
'''

# Characters a client may put in a query string or a path segment as they are (RFC 3986 pchar/query).
SAFE = "abcxyzABC019-._~!$'()*,:@"
PIECES = ["%", "%2", "%41", "%4g", "%g4", "%%", "%-1", "% 1", "%+1", "%2B", "%26", "%3D", "%3d", "%25",
          "%20", "%00", "%C3%A7", "%c3%a7", "%E2%82%AC", "%F0%9F%98%80", "%FF", "%C3", "%C0%80",
          "%ED%A0%80", "+", "++", "=", "==", ";", "?", "/", "a", "key", "k1", "x"]
URL_PIECES = [p for p in PIECES if " " not in p]   # a space cannot be sent in a request line


# ── model ────────────────────────────────────────────────────────────────────
def is_hex(c):
    return c in "0123456789abcdefABCDEF"


def url_decode(s, plus_is_space=True):
    out = bytearray()
    i = 0
    b = s.encode("latin-1")
    while i < len(b):
        c = b[i]
        if c == 0x2B:
            out += b" " if plus_is_space else b"+"
        elif c == 0x25 and i + 2 < len(b) and is_hex(chr(b[i + 1])) and is_hex(chr(b[i + 2])):
            out.append(int(b[i + 1:i + 3].decode(), 16))
            i += 2
        else:
            out.append(c)
        i += 1
    return bytes(out)


def utf8_len(b, i):
    """Length of the valid UTF-8 sequence at b[i], 0 if none (Go's utf8.DecodeRune rules)."""
    def at(k):
        return b[k] if k < len(b) else 0x100

    def cont(k, lo=0x80, hi=0xBF):
        return lo <= at(k) <= hi
    c = at(i)
    if c < 0x80:
        return 1
    if 0xC2 <= c <= 0xDF:
        return 2 if cont(i + 1) else 0
    if c == 0xE0:
        return 3 if cont(i + 1, 0xA0) and cont(i + 2) else 0
    if 0xE1 <= c <= 0xEC or c in (0xEE, 0xEF):
        return 3 if cont(i + 1) and cont(i + 2) else 0
    if c == 0xED:
        return 3 if cont(i + 1, 0x80, 0x9F) and cont(i + 2) else 0
    if c == 0xF0:
        return 4 if cont(i + 1, 0x90) and cont(i + 2) and cont(i + 3) else 0
    if 0xF1 <= c <= 0xF3:
        return 4 if cont(i + 1) and cont(i + 2) and cont(i + 3) else 0
    if c == 0xF4:
        return 4 if cont(i + 1, 0x80, 0x8F) and cont(i + 2) and cont(i + 3) else 0
    return 0


def as_json_text(b):
    """The str a json::encode of these bytes reads back as: invalid UTF-8 -> U+FFFD per byte."""
    out = []
    i = 0
    while i < len(b):
        n = utf8_len(b, i)
        if n == 0:
            out.append("�")
            i += 1
        else:
            out.append(b[i:i + n].decode("utf-8"))
            i += n
    return "".join(out)


def parse_form(s):
    res = {}
    if not s:
        return res
    pieces = s.split("&")
    if pieces and pieces[-1] == "":
        pieces = pieces[:-1]                  # std::getline gives no empty piece at the very end
    for p in pieces:
        if p == "":
            continue
        k, eq, v = p.partition("=")
        res[url_decode(k)] = url_decode(v) if eq else b""
    return res


def expect_all(query, body):
    g = parse_form(query)
    p = parse_form(body) if body is not None else {}
    merged = dict(g)
    merged.update(p)
    # As (name, value) pairs: two distinct names that are both invalid UTF-8 ("%FF", "%C3") are two
    # entries of the map, and json::encode writes both names as U+FFFD (Go's json.Marshal does too).
    return sorted((as_json_text(k), as_json_text(v)) for k, v in merged.items())


# ── generator ────────────────────────────────────────────────────────────────
def gen_token(r, pieces):
    n = r.randint(0, 4)
    out = ""
    for _ in range(n):
        out += r.choice(pieces) if r.random() < 0.6 else r.choice(SAFE)
    return out


def gen_query(r, pieces=URL_PIECES):
    parts = []
    for _ in range(r.randint(0, 5)):
        k = gen_token(r, pieces)
        if r.random() < 0.15:
            parts.append(k)                    # no '='
        else:
            parts.append(k + "=" + gen_token(r, pieces))
    if parts and r.random() < 0.3:
        parts.append(r.choice(parts))          # a repeated name
    sep = "&" if r.random() < 0.85 else r.choice(["&&", "&", "&;"])
    q = sep.join(parts)
    if r.random() < 0.1:
        q = "&" + q
    if r.random() < 0.1:
        q += "&"
    return q


def gen_segment(r):
    s = ""
    for _ in range(r.randint(1, 5)):
        p = r.choice(URL_PIECES) if r.random() < 0.6 else r.choice(SAFE)
        s += p.replace("/", "").replace("?", "")
    return s or "a"


# ── server ───────────────────────────────────────────────────────────────────
def start(fcgi, port, tmp):
    app = os.path.join(tmp, "app.lk")
    with open(app, "w") as f:
        f.write(APP)
    log = open(os.path.join(tmp, "server.log"), "w")
    p = subprocess.Popen([fcgi, "--mode", "http", "--port", str(port), app], stdout=log, stderr=log)
    for _ in range(100):
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            c.request("GET", "/q")
            c.getresponse().read()
            return p
        except OSError:
            time.sleep(0.1)
    p.kill()
    raise SystemExit("FAIL: the server did not start (see %s)" % log.name)


def ask(port, method, target, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    c.request(method, target, body=body.encode("latin-1") if body is not None else None, headers=headers)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data


def pairs(p):
    return ("__pairs__", p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fcgi")
    ap.add_argument("--cases", type=int, default=int(os.environ.get("LOOK_FORM_FUZZ_CASES", "1500")))
    ap.add_argument("--seed", type=int, default=20261120)
    ap.add_argument("--port", type=int, default=7561)
    ap.add_argument("--show", type=int, default=2)
    a = ap.parse_args()
    r = random.Random(a.seed)
    kinds = {}

    def fail(kind, example, detail):
        kinds.setdefault(kind, []).append((example, detail))

    with tempfile.TemporaryDirectory() as tmp:
        srv = start(os.path.abspath(a.fcgi), a.port, tmp)
        try:
            for n in range(a.cases):
                kind = r.random()
                if kind < 0.45:
                    q = gen_query(r)
                    target, body, method = "/q?" + q if q else "/q", None, "GET"
                    want = expect_all(q, None)
                elif kind < 0.8:
                    q = gen_query(r) if r.random() < 0.5 else ""
                    body = gen_query(r, PIECES)
                    target, method = ("/q?" + q) if q else "/q", "POST"
                    want = expect_all(q, body)
                else:
                    seg = gen_segment(r)
                    target, body, method = "/p/" + seg, None, "GET"
                    v = as_json_text(url_decode(seg, plus_is_space=False))
                    want = None
                status, data = ask(a.port, method, target, body)
                text = data.decode("utf-8", "surrogateescape")
                example = "%s %s%s" % (method, target, ("  body=" + body) if body is not None else "")
                if status != 200:
                    fail("status %d" % status, example, text[:200])
                    continue
                try:
                    doc = json.loads(text, object_pairs_hook=pairs)
                except ValueError:
                    fail("response is not valid JSON", example, text[:200])
                    continue
                d = dict(doc[1])
                if want is None:
                    if d.get("arg") != v or d.get("param") != v:
                        fail("route parameter", example, "got arg=%r param=%r, want %r" % (d.get("arg"), d.get("param"), v))
                    continue
                got = d["all"]
                if got == [] and not want:
                    continue
                if not (isinstance(got, tuple) and got[0] == "__pairs__"):
                    fail("request::all() is not a map", example, repr(got)[:200])
                    continue
                keys = [k for k, _ in got[1]]
                wkeys = [k for k, _ in want]
                if len(keys) != len(set(keys)) and len(wkeys) == len(set(wkeys)):
                    fail("request::all() holds a name twice", example, repr(keys))
                    continue
                if sorted(got[1]) != want:
                    fail("request::all() differs from the model", example,
                         "got %r, want %r" % (sorted(got[1]), want))
        finally:
            srv.kill()
            srv.wait()

    print("form differential: %d requests, seed %d" % (a.cases, a.seed))
    if not kinds:
        print("PASS: query strings, form bodies and route parameters decode as the model says")
        return 0
    for k, items in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print("FAIL (%d requests): %s" % (len(items), k))
        for ex, d in sorted(items, key=lambda x: len(x[0]))[:a.show]:
            print("    %s" % ex)
            print("      -> %s" % d[:300])
    return 1


if __name__ == "__main__":
    sys.exit(main())

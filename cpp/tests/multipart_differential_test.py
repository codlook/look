#!/usr/bin/env python3
# multipart/form-data differential fuzz test: random bodies - valid ones, edge cases and the shapes a parser can read
# differently (a boundary prefix inside the data, delimiter lines with trailing spaces, a preamble that mentions the
# boundary, bare LF, a missing closing delimiter, odd Content-Disposition headers, repeated parameters, ...) - go to
# `lk-fcgi --mode http` and to Go's mime/multipart (tests/input_fuzz/multipart_referee.go). Each answers with the
# fields and files it read; the answers are compared.
#
# Each difference is classified by direction:
#   LOOSER     LOOK accepts a body Go refuses (the field and file set it read are not the one Go would stop at): FAIL.
#   DIFFERENT  both accept but read other fields or files (a name, a value, a file name or size): FAIL - a body that
#              a proxy or a WAF reads with one parser and the application with another is how uploads get smuggled.
#   STRICTER   LOOK refuses what Go reads. Not a failure; counted and listed (a few examples).
#
# Usage: python3 tests/multipart_differential_test.py <lk-fcgi> [--cases N] [--seed S] [--referee BIN]
# Without --referee the referee is built with `go build` (Go must be on PATH). Default: 2000 bodies, fixed seed.
import argparse
import http.client
import json
import os
import random
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = ["a", "b", "f", "g", "c d", "é"]   # the file names the LOOK app looks for
APP = '''
route("POST", "/m", function() {
    $files = []
    foreach (%s as $n) {
        $f = request::file($n)
        if ($f != null) { $files[$n] = [$f["filename"], $f["size"]] }
    }
    return response::json(["post" => request::all(), "files" => $files])
})
''' % json.dumps(NAMES, ensure_ascii=False)

FIELD_NAMES = NAMES + ["x", "y1", "n_2"]
VALUES = [b"", b"v", b"hello world", b"line1\r\nline2", b"\x00\x01\x02", b"a=3Db", b"--", b"\r\n", b"caf\xc3\xa9", b"x" * 300]
FILENAMES = ["a.txt", "b.png", "dir/c.txt", "d e.txt", "ü.txt", "x"]


def cd(kind, name, fn):
    # (header value, expects_field_name, expects_filename) - the model only labels; the referee decides
    n, f = name, fn
    forms = {
        "plain": 'form-data; name="%s"' % n,
        "file": 'form-data; name="%s"; filename="%s"' % (n, f),
        "swap": 'form-data; filename="%s"; name="%s"' % (f, n),
        "caps": 'FORM-DATA; name="%s"' % n,
        "token": "form-data; name=%s" % n.replace(" ", "_"),
        "tight": 'form-data;name="%s";filename="%s"' % (n, f),
        "dupname": 'form-data; name="%s"; name="b"' % n,
        "attachment": 'attachment; name="%s"' % n,
        "inline": 'inline; name="%s"; filename="%s"' % (n, f),
        "star": "form-data; name=\"%s\"; filename*=UTF-8''%s" % (n, f),
        "esc": 'form-data; name="%s\\"x"' % n,
        "emptyfn": 'form-data; name="%s"; filename=""' % n,
        "dupfn": 'form-data; name="%s"; filename="%s"; filename="z"' % (n, f),
        "spaces": 'form-data ; name = "%s" ; filename = "%s"' % (n, f),
        "nested": 'form-data; name="%s; filename=%s"' % (n, f),
        "noname": 'form-data; filename="%s"' % f,
        "bare": "form-data",
        "unclosed": 'form-data; name="%s' % n,
    }
    return forms[kind]


KINDS = ["plain"] * 8 + ["file"] * 8 + ["swap", "caps", "token", "tight", "dupname", "attachment", "inline", "star",
                                          "esc", "emptyfn", "dupfn", "spaces", "nested", "noname", "bare", "unclosed"]


def gen(r):
    B = r.choice(["B", "----WebKitFormBoundary7MA4YWxkTrZu0gW", "a'b(c)+_,-./:=?", "xyz", "b" * 70])
    parts = []
    tags = set()
    for _ in range(r.choice([0, 1, 1, 2, 2, 3, 4])):
        kind = r.choice(KINDS)
        name = r.choice(FIELD_NAMES if kind not in ("file", "swap", "tight", "inline", "star", "spaces", "dupfn", "emptyfn")
                        else NAMES)
        fn = r.choice(FILENAMES)
        hdr_name = r.choice(["Content-Disposition"] * 6 + ["content-disposition", "CONTENT-DISPOSITION"])
        headers = [hdr_name + ": " + cd(kind, name, fn)]
        if kind not in ("plain", "file"):
            tags.add("cd-" + kind)
        if r.random() < 0.15:
            headers.append("Content-Type: text/plain")
        if r.random() < 0.06:
            headers.append("Content-Transfer-Encoding: quoted-printable"); tags.add("cte-qp")
        if r.random() < 0.05:
            headers.insert(0, "X-Junk: " + "j" * r.choice([5, 200, 5000])); tags.add("junk-header")
        if r.random() < 0.04:
            headers.append(" continuation"); tags.add("fold")
        if r.random() < 0.03:
            headers.append("no colon here"); tags.add("bad-header")
        data = r.choice(VALUES)
        if r.random() < 0.10:
            data = data + b"\r\n--" + B.encode() + r.choice([b"ZZ", b"X--", b"-", b" tail", b"\t"]) + b"more"
            tags.add("prefix-in-data")
        if r.random() < 0.04:
            data = b"\n--" + B.encode() + b"\n" + data; tags.add("lf-delim-in-data")
        parts.append((headers, data))
    out = bytearray()
    pre = b""
    if r.random() < 0.15:
        pre = r.choice([b"preamble\r\n", b"x --" + B.encode() + b" y\r\n", b"\r\n", b"junk"]); tags.add("preamble")
        if pre.startswith(b"x --"): tags.add("preamble-mentions-boundary")
    out += pre
    for headers, data in parts:
        line = b"--" + B.encode()
        if r.random() < 0.06:
            line += r.choice([b" ", b"\t", b"  \t", b"X"]); tags.add("delim-trailing")
        out += line + b"\r\n"
        out += "\r\n".join(headers).encode("utf-8") + b"\r\n\r\n" + data + b"\r\n"
    close = b"--" + B.encode() + b"--"
    if r.random() < 0.05:
        close += r.choice([b" ", b" junk", b"\t"]); tags.add("close-trailing")
    if r.random() < 0.06:
        tags.add("no-close")
    else:
        out += close + r.choice([b"\r\n", b"\r\n", b"", b"\r\nepilogue", b"\n"])
    body = bytes(out) or b"x"   # an empty body is not a multipart body for Go and LOOK alike; not what this test is about
    if r.random() < 0.05:
        body = body.replace(b"\r\n", b"\n"); tags.add("lf-only")
    return B, body, tags


def start(fcgi, port, tmp):
    app = os.path.join(tmp, "app.lk")
    with open(app, "w", encoding="utf-8") as f:
        f.write(APP)
    log = open(os.path.join(tmp, "server.log"), "w")
    env = dict(os.environ, LOOK_SECRET=os.environ.get("LOOK_SECRET", "multipart-fuzz-secret-0123456789abcdef0123"))
    p = subprocess.Popen([fcgi, "--mode", "http", "--port", str(port), app], stdout=log, stderr=log, env=env)
    return p


def wait(port, p, what):
    for _ in range(100):
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            c.request("GET", "/none")
            c.getresponse().read()
            return
        except OSError:
            if p.poll() is not None:
                break
            time.sleep(0.1)
    raise SystemExit("%s did not start" % what)


def post(port, boundary, body):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    token = all(c.isalnum() or c in "!#$%&'*+-.^_`|~" for c in boundary)   # a compliant client quotes any other boundary
    param = boundary if token else '"' + boundary + '"'
    c.request("POST", "/m", body=body, headers={"Content-Type": "multipart/form-data; boundary=" + param})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data


def norm(status, data):
    """-> ("err", text) or ("ok", post dict, files dict)"""
    if status != 200:
        return ("err", "status %d" % status)
    try:
        d = json.loads(data.decode("utf-8", "replace"))
    except ValueError:
        return ("err", "not JSON: %r" % data[:60])
    if "error" in d:
        return ("err", d["error"])
    p, f = d.get("post"), d.get("files")
    p = {} if p in ([], None) else p
    f = {} if f in ([], None) else f
    return ("ok", {k: v for k, v in p.items()}, {k: list(v) for k, v in f.items()})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fcgi")
    ap.add_argument("--cases", type=int, default=int(os.environ.get("LOOK_MULTIPART_FUZZ_CASES", "2000")))
    ap.add_argument("--seed", type=int, default=20261124)
    ap.add_argument("--referee")
    ap.add_argument("--look-port", type=int, default=7581)
    ap.add_argument("--go-port", type=int, default=7582)
    ap.add_argument("--show", type=int, default=3)
    a = ap.parse_args()
    r = random.Random(a.seed)
    found = {}
    counts = {"same": 0, "both-refuse": 0}
    with tempfile.TemporaryDirectory() as tmp:
        exe = a.referee
        if not exe:
            exe = os.path.join(tmp, "multipart_referee")
            subprocess.run(["go", "build", "-o", exe, os.path.join(HERE, "input_fuzz", "multipart_referee.go")], check=True,
                           env=dict(os.environ, CGO_ENABLED="0", GO111MODULE="off"))
        lk = start(os.path.abspath(a.fcgi), a.look_port, tmp)
        go = subprocess.Popen([exe, str(a.go_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait(a.look_port, lk, "lk-fcgi")
            wait(a.go_port, go, "the Go referee")
            for _ in range(a.cases):
                B, body, tags = gen(r)
                ls, ld = post(a.look_port, B, body)
                gs, gd = post(a.go_port, B, body)
                L, G = norm(ls, ld), norm(gs, gd)
                if lk.poll() is not None:
                    found.setdefault(("CRASH", "-"), []).append((B, body, L, G))
                    break
                if L[0] == "err" and G[0] == "err":
                    counts["both-refuse"] += 1
                    continue
                key = ",".join(sorted(tags)) or "-"
                if L[0] == "ok" and G[0] == "err":
                    found.setdefault(("LOOSER", key), []).append((B, body, L, G))
                elif L[0] == "err" and G[0] == "ok":
                    found.setdefault(("STRICTER", "cte-qp: refused on purpose (Go decodes it)" if "cte-qp" in tags else key), []).append((B, body, L, G))
                elif L[1:] != G[1:]:
                    found.setdefault(("DIFFERENT", key), []).append((B, body, L, G))
                else:
                    counts["same"] += 1
        finally:
            lk.kill(); lk.wait()
            go.kill(); go.wait()
    bad = 0
    print("multipart differential: %d bodies, seed %d: same reading %d, both refuse %d" % (
        a.cases, a.seed, counts["same"], counts["both-refuse"]))
    for (kind, key), items in sorted(found.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
        fatal = kind != "STRICTER"
        bad += len(items) if fatal else 0
        print("%s (%d): %s" % (kind, len(items), key))
        for B, body, L, G in sorted(items, key=lambda x: len(x[1]))[:a.show if fatal else 1]:
            print("    boundary=%s body=%r" % (B[:20], body[:260]))
            print("      LOOK: %s" % (L,))
            print("      Go:   %s" % (G,))
    if bad:
        print("FAIL: %d bodies are read differently from Go's mime/multipart" % bad)
        return 1
    print("PASS: multipart bodies are read as Go reads them (LOOK refuses more in the STRICTER rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

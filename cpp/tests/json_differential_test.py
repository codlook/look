#!/usr/bin/env python3
# JSON differential fuzz test: random JSON texts (valid, at the edges, and broken) go through
# LOOK's json::decode and through Go's encoding/json (tests/input_fuzz/json_referee.go). The
# documentation promises that json::decode accepts exactly what RFC 8259 and Go accept, so Go is
# the referee for what a text means:
#   * Go rejects the text            -> json::decode returns null
#   * Go accepts it                  -> json::decode returns the same value:
#       - an integer that fits in signed 64 bits is an int; a larger one is the exact text, as a string
#       - any other number is the float Go reads (same bits, sign of zero included)
#       - strings are equal after Go's decoding (invalid UTF-8 and lone surrogates -> U+FFFD)
#       - an object has the same keys and values; a duplicated key keeps its first position and
#         takes the last value
#       - nesting deeper than 256 levels is null (LOOK's documented limit; Go allows 10000)
# The value is read back through json::encode, so a json::encode that loses a float digit or
# escapes a string wrongly fails here too.
#
# Usage:
#   python3 tests/json_differential_test.py ./build/lk [--cases N] [--seed S] [--referee BIN]
# Without --referee the referee is built with `go build` (Go must be on PATH).
# Default: 4000 texts, fixed seed (deterministic). LOOK_JSON_FUZZ_CASES=N runs more.
import argparse
import base64
import json
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1
MAX_DEPTH = 256


# ── generator ────────────────────────────────────────────────────────────────
WS_OK = [b" ", b"\t", b"\n", b"\r"]
WS_BAD = [b"\x0c", b"\x0b", b"\xc2\xa0", b"\xef\xbb\xbf", b"\x00"]

NUM_EDGE = [
    "0", "-0", "1", "-1", "10", "123456789", "9223372036854775807", "-9223372036854775808",
    "9223372036854775808", "-9223372036854775809", "18446744073709551615", "18446744073709551616",
    "123456789012345678901234567890", "-99999999999999999999999",
    "0.0", "-0.0", "0.1", "1.5", "-2.25", "1e0", "1E5", "1e+5", "1e-5", "2.5E-3", "1.0e10",
    "1.7976931348623157e308", "2.2250738585072014e-308", "5e-324", "4.9e-324", "1e-400",
    "123.456e-7", "0e0", "0E-0", "-0e+00", "9007199254740993", "9007199254740993.0",
    "0.30000000000000004", "1.7976931348623159e308", "1e309", "1e999", "-1e999",
]
NUM_BAD = ["01", "-01", "1.", ".5", "+1", "-", "1e", "1e+", "--1", "0x10", "1_000", "NaN",
           "Infinity", "-Infinity", "1.e5", "00", "1ee5", "1e5.5", "١"]

STR_PIECES = [
    b"a", b"abc", b"hello world", b"\\n", b"\\t", b"\\r", b"\\b", b"\\f", b"\\/", b"\\\\", b'\\"',
    b"\\u0041", b"\\u00e9", b"\\u0000", b"\\u001f", b"\\u007f", b"\\u2028", b"\\uFFFF", b"\\uffff",
    b"\\ud83d\\ude00", b"\\uD83D\\uDE00", b"\\ud800", b"\\udc00", b"\\ud800\\u0041", b"\\udbff\\udfff",
    b"\\ud800\\ud800", "é".encode(), "çğış".encode(), "日本".encode(), "😀".encode(), b"\x7f",
    b"\xff", b"\xc3", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82",
    b"0", b"123", b"true", b"<>&", b"'", b"%41", b"+",
]
STR_BAD = [b"\\x", b"\\u12", b"\\uZZZZ", b"\\U0041", b"\\", b"\x01", b"\n", b"\t", b"\x1f", b"\\a", b"\\0"]

KEY_POOL = [b"a", b"b", b"key", b"", b"0", b"1", b"2", b"-1", b"01", b"10", b"__proto__", b"\\u0000",
            b"k\\u00e9y", "çğ".encode(), b"\\ud800", b"a b", b"length", b"\\n"]


def gen_number(r):
    if r.random() < 0.6:
        return r.choice(NUM_EDGE).encode()
    kind = r.random()
    if kind < 0.4:
        return str(r.randint(-10**6, 10**6)).encode()
    if kind < 0.6:
        return str(r.choice([-1, 1]) * r.randint(0, 1 << r.choice([31, 53, 62, 63, 64, 70]))).encode()
    if kind < 0.8:
        return repr(r.uniform(-1e6, 1e6)).encode()
    # random float bits: every kind of finite double
    while True:
        f = struct.unpack("<d", struct.pack("<Q", r.getrandbits(64)))[0]
        if math.isfinite(f):
            return repr(f).encode()


def gen_string(r):
    n = r.choice([0, 1, 1, 2, 3, 5])
    return b'"' + b"".join(r.choice(STR_PIECES) for _ in range(n)) + b'"'


def ws(r):
    if r.random() < 0.7:
        return b""
    return b"".join(r.choice(WS_OK) for _ in range(r.randint(1, 3)))


def gen_value(r, depth):
    k = r.random()
    if depth > 5 or k < 0.45:
        s = r.random()
        if s < 0.35:
            return gen_number(r)
        if s < 0.75:
            return gen_string(r)
        return r.choice([b"true", b"false", b"null"])
    if k < 0.72:
        items = [ws(r) + gen_value(r, depth + 1) + ws(r) for _ in range(r.randint(0, 4))]
        return b"[" + b",".join(items) + ws(r) + b"]"
    pairs = []
    for _ in range(r.randint(0, 4)):
        key = r.choice(KEY_POOL) if r.random() < 0.8 else gen_string(r)[1:-1]
        pairs.append(ws(r) + b'"' + key + b'"' + ws(r) + b":" + ws(r) + gen_value(r, depth + 1) + ws(r))
    if pairs and r.random() < 0.25:          # a duplicated key
        pairs.append(r.choice(pairs).split(b":", 1)[0] + b":" + gen_value(r, depth + 1))
    return b"{" + b",".join(pairs) + ws(r) + b"}"


def mutate(r, t):
    m = r.random()
    if not t:
        return r.choice(NUM_BAD).encode()
    i = r.randrange(len(t) + 1)
    if m < 0.15:
        return t[:i] + t[i + 1:]
    if m < 0.30:
        return t[:i] + bytes([r.randrange(256)]) + t[i:]
    if m < 0.40:
        return t[:i]
    if m < 0.50:
        return t + r.choice([b"x", b",", b" 1", b"]", b"}", b"null", b"\x00"])
    if m < 0.60:
        return t.replace(b"]", b",]", 1) if b"]" in t else t.replace(b"}", b",}", 1)
    if m < 0.68:
        return t.replace(b":", b"", 1)
    if m < 0.76:
        return r.choice(WS_BAD) + t if r.random() < 0.5 else t + r.choice(WS_BAD)
    if m < 0.86:
        return t[:i] + r.choice(NUM_BAD).encode() + t[i:]
    if m < 0.94:
        return b'"' + r.choice(STR_BAD) + b'"'
    return t.replace(b'"', b"'", 2)


def depth_cases():
    out = []
    for d in (1, 2, 255, 256, 257, 300, 1000, 10001):
        out.append(b"[" * d + b"]" * d)
        out.append(b'{"a":' * d + b"1" + b"}" * d)
        out.append(b"[" * d + b'{"x":[]}' + b"]" * d)
    out.append(b"[" * 100000)                 # unterminated, very deep
    return out


def gen_cases(r, n):
    cases = depth_cases()
    cases += [e.encode() for e in NUM_EDGE + NUM_BAD]
    cases += [b'"' + p + b'"' for p in STR_PIECES + STR_BAD]
    cases += [b"", b" ", b"{}", b"[]", b'{"":""}', b'{"a":1,"a":2}', b'{"a":1,"b":2,"a":3}',
              b'{"0":"x","1":"y"}', b'{"1":"y","0":"x"}', b'[[],{}]', b'{"a":{}}', b'{"a":[]}']
    while len(cases) < n:
        t = gen_value(r, 0)
        t = ws(r) + t + ws(r)
        if r.random() < 0.35:
            t = mutate(r, t)
        cases.append(t)
    return cases


# ── referee ──────────────────────────────────────────────────────────────────
def build_referee(tmp):
    exe = os.path.join(tmp, "json_referee")
    src = os.path.join(HERE, "input_fuzz", "json_referee.go")
    env = dict(os.environ, CGO_ENABLED="0", GO111MODULE="off")
    subprocess.run(["go", "build", "-o", exe, src], check=True, env=env)
    return exe


def run_referee(exe, cases):
    data = b"".join(base64.b64encode(c) + b"\n" for c in cases)
    p = subprocess.run([exe], input=data, stdout=subprocess.PIPE, check=True)
    lines = p.stdout.decode("utf-8").split("\n")
    assert len(lines) == len(cases) + 1, "referee answered %d of %d" % (len(lines) - 1, len(cases))
    return [None if l == "X" else l for l in lines[:-1]]     # parsed lazily (deep texts are not)


# ── LOOK ─────────────────────────────────────────────────────────────────────
def run_look(lk, cases, tmp):
    prog = os.path.join(tmp, "json_cases.lk")
    with open(prog, "w") as f:
        f.write("use crypto\n$in = [\n")
        f.write(",\n".join('"%s"' % base64.b64encode(c).decode() for c in cases))
        f.write("\n]\n$i = 0\nforeach ($in as $b) {\n")
        f.write("    $v = json::decode(crypto::base64_decode($b))\n")
        f.write("    try {\n        print(\"#\" . $i . \" \" . json::encode([\"v\" => $v]))\n")
        f.write("    } catch ($e) {\n        print(\"#\" . $i . \" ENCODE-ERROR \" . $e)\n    }\n")
        f.write("    $i = $i + 1\n}\nprint(\"#end\")\n")
    p = subprocess.run([lk, prog], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    out = p.stdout.decode("utf-8", "surrogateescape").split("\n")
    if p.returncode != 0 or "#end" not in out:
        sys.stderr.write(p.stderr.decode("utf-8", "replace")[-2000:])
        raise SystemExit("FAIL: the LOOK program did not finish (exit %d)" % p.returncode)
    res = {}
    for line in out:
        m = re.match(r"#(\d+) (.*)$", line)
        if m:
            res[int(m.group(1))] = m.group(2)
    return res


def look_int(t):
    # json::encode writes the float -0.0 as -0 (as Go does); Python would read it as the int 0.
    return -0.0 if t == "-0" else int(t)


def pairs_hook(p):
    return ("__pairs__", p)


def lone_surrogates_to_fffd(s):
    return re.sub("[\ud800-\udfff]", "�", s)


def expected_order(text):
    """Key order of every object in `text` as Python reads it (first occurrence). None if unknown."""
    try:
        s = text.decode("utf-8")
        return json.loads(s, object_pairs_hook=pairs_hook, parse_constant=lambda c: 0)
    except Exception:
        return None


def same_float(a, b):
    return a == b and math.copysign(1, a) == math.copysign(1, b)


def compare(tree, act, order, path="$"):
    """None when equal, else a description of the first difference."""
    if tree is None or isinstance(tree, bool):
        if act is tree:
            return None
        return "%s: want %s, got %r" % (path, json.dumps(tree), act)
    if "n" in tree:
        t = tree["n"]
        if re.fullmatch(r"-?\d+", t):
            v = int(t)
            if INT64_MIN <= v <= INT64_MAX:
                if type(act) is int and act == v:
                    return None
                return "%s: number %s should be the int %d, got %r" % (path, t, v, act)
            if act == t:
                return None
            return "%s: number %s is beyond int64 and should be the string %r, got %r" % (path, t, t, act)
        f = float(t)
        if type(act) in (int, float) and not isinstance(act, bool) and same_float(float(act), f):
            return None
        return "%s: number %s should be the float %r, got %r" % (path, t, f, act)
    if "s" in tree:
        if act == tree["s"]:
            return None
        return "%s: string should be %r, got %r" % (path, tree["s"], act)
    if "a" in tree:
        want = tree["a"]
        if isinstance(act, tuple) and act[0] == "__pairs__" and len(act[1]) == 0 and not want:
            return "%s: [] came back as {}" % path
        if not isinstance(act, list):
            return "%s: want an array of %d, got %s" % (path, len(want), short(act))
        if len(act) != len(want):
            return "%s: array length %d, want %d" % (path, len(act), len(want))
        for i, (w, a) in enumerate(zip(want, act)):
            o = order[i] if isinstance(order, list) and i < len(order) else None
            d = compare(w, a, o, "%s[%d]" % (path, i))
            if d:
                return d
        return None
    want = tree["o"]
    if isinstance(act, list) and not act and not want:
        return "%s: {} came back as []" % path
    if not (isinstance(act, tuple) and act[0] == "__pairs__"):
        return "%s: want an object with keys %s, got %s" % (path, sorted(want)[:6], short(act))
    keys = [k for k, _ in act[1]]
    if len(set(keys)) != len(keys):
        return "%s: duplicated key in the output: %r" % (path, keys)
    if set(keys) != set(want):
        return "%s: keys %r, want %r" % (path, keys, sorted(want))
    if isinstance(order, tuple) and order[0] == "__pairs__":
        first = []
        for k, _ in order[1]:
            k = lone_surrogates_to_fffd(k)
            if k not in first:
                first.append(k)
        if set(first) == set(keys) and first != keys:
            return "%s: key order %r, want first-occurrence order %r" % (path, keys, first)
        last = {}
        for k, v in order[1]:
            last[lone_surrogates_to_fffd(k)] = v
    else:
        last = {}
    for k, a in act[1]:
        d = compare(want[k], a, last.get(k), "%s[%s]" % (path, json.dumps(k)))
        if d:
            return d
    return None


def short(v):
    s = repr(v)
    return s if len(s) < 80 else s[:77] + "..."


def nesting(text):
    depth = best = 0
    in_str = esc = False
    for c in text:
        if in_str:
            if esc:
                esc = False
            elif c == 0x5C:
                esc = True
            elif c == 0x22:
                in_str = False
        elif c == 0x22:
            in_str = True
        elif c in (0x5B, 0x7B):
            depth += 1
            best = max(best, depth)
        elif c in (0x5D, 0x7D):
            depth -= 1
    return best


def judge(text, tree, look_line):
    if look_line.startswith("ENCODE-ERROR"):
        return "json::encode failed on the decoded value: " + look_line
    try:
        doc = json.loads(look_line, object_pairs_hook=pairs_hook, parse_int=look_int)
        got = dict(doc[1])["v"]
    except Exception as e:
        return "LOOK's json::encode output is not valid JSON (%s): %r" % (e, look_line[:200])
    if tree is not None and nesting(text) <= MAX_DEPTH:
        tree = json.loads(tree)
    if tree is None:
        if got is None:
            return None
        return "Go rejects this text, json::decode accepted it as %s" % short(got)
    if nesting(text) > MAX_DEPTH:
        return None if got is None else "nesting %d > %d should be null, got %s" % (nesting(text), MAX_DEPTH, short(got))
    if got is None and tree is not None:
        return "Go accepts this text (%s), json::decode returned null" % short(tree)[:120]
    return compare(tree, got, expected_order(text))


def classify(desc):
    """The kind of a difference: its description without the path and the values."""
    d = re.sub(r"^\$\S*: ", "", desc)
    d = re.sub(r"\(.*\)", "", d)
    d = re.sub(r"'[^']*'|\"[^\"]*\"|\[.*\]|\{.*\}|-?\d+(\.\d+)?(e[-+]?\d+)?", "_", d)
    return d[:90]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lk")
    ap.add_argument("--cases", type=int, default=int(os.environ.get("LOOK_JSON_FUZZ_CASES", "4000")))
    ap.add_argument("--seed", type=int, default=20261119)
    ap.add_argument("--referee", help="a built json_referee (else `go build`)")
    ap.add_argument("--show", type=int, default=3, help="examples printed per kind of difference")
    a = ap.parse_args()
    sys.setrecursionlimit(100000)
    r = random.Random(a.seed)
    cases = gen_cases(r, a.cases)
    with tempfile.TemporaryDirectory() as tmp:
        exe = a.referee or build_referee(tmp)
        trees = run_referee(exe, cases)
        look = run_look(os.path.abspath(a.lk), cases, tmp)
    kinds = {}
    for i, (text, tree) in enumerate(zip(cases, trees)):
        if i not in look:
            d = "no output from LOOK"
        else:
            d = judge(text, tree, look[i])
        if d:
            kinds.setdefault(classify(d), []).append((text, d))
    accepted = sum(1 for t in trees if t is not None)
    print("json differential: %d texts (%d valid per Go, %d invalid), seed %d"
          % (len(cases), accepted, len(cases) - accepted, a.seed))
    if not kinds:
        print("PASS: json::decode agrees with Go's encoding/json on every text")
        return 0
    for k, items in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print("FAIL (%d texts): %s" % (len(items), items[0][1]))
        for text, d in sorted(items, key=lambda x: len(x[0]))[:a.show]:
            print("    input %r" % (text[:160],))
            print("      -> %s" % d[:300])
    return 1


if __name__ == "__main__":
    sys.exit(main())

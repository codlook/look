#!/usr/bin/env python3
# Collection differential fuzz test: generates random LOOK programs (list / map / string
# operations), runs them with the real `lk`, and executes the same operation sequence in a
# simple, obviously correct reference model written here. Any difference = FAIL; the differing
# program is shrunk statement by statement and the smallest form is printed.
#
# Usage:
#   python3 tests/collections_differential_test.py ./build/lk [--iterations N] [--seed S]
#   LOOK_COLL_FUZZ_ITERATIONS=5000 python3 tests/collections_differential_test.py ./build/lk
# Default: 600 programs (~30 s), fixed seed (deterministic).
#
# Rules of the model (docs/look.codlook.com/docs.html + value_semantics_test.lk):
#   * Arrays and maps are VALUES: `$b = $a` gives an independent copy (copy-on-write);
#     a function parameter is a copy too.
#   * A map keeps insertion order; writing to an existing key does not change its position.
#   * A map key is text: $m[3] and $m["3"] are the same key; "03", "+3" are separate keys.
#   * List index: an integer or integer TEXT ("1", "+1", "01" -> 1); negative = from the end;
#     reading out of range = error; writing at position len = append, beyond = error.
#   * Writing to a list with a non-integer text key turns the list into a map (with the keys "0","1",…)
#     — the behavior of the code (vm.cpp array_set).
#   * Reading a non-integer text key on a list gives null; a missing key in a map gives null.
#   * push/pop only on a list; error on a map. pop on an empty list gives null. The new form (take):
#     $x = $l[-1] + $l = array::slice($l, 0, -1) — error on an empty list and on a map; a bare pop is rarer.
#   * == compares contents: lists ordered, maps unordered; no string/number conversion.
#   * string::len/substr/reverse/index_of work on Unicode code points.
import argparse
import json
import os
import random
import re
import subprocess
import sys
import tempfile
import time


class Invalid(Exception):
    """Bu program modelde geçersiz (üretici/küçültücü bu adayı atlar)."""


class LookErr(Exception):
    """LOOK'ta hata vermesi BEKLENEN işlem (try/catch içinde 'ERR' basılır)."""


# ── value model ──────────────────────────────────────────────────────────────
# list -> Python list ; map -> MapV (ordered dict) ; scalar: int / str / bool / None
class MapV:
    __slots__ = ("d",)

    def __init__(self, d=None):
        self.d = dict(d or {})

    def copy(self):
        return MapV({k: deep(v) for k, v in self.d.items()})


def deep(v):
    if isinstance(v, list):
        return [deep(x) for x in v]
    if isinstance(v, MapV):
        return v.copy()
    return v


def is_int_key(s):
    if not s:
        return False
    i = 1 if s[0] in "+-" else 0
    return i < len(s) and all("0" <= c <= "9" for c in s[i:])


def key_str(k):
    if isinstance(k, bool):
        raise Invalid()
    if isinstance(k, int):
        return str(k)
    if isinstance(k, str):
        return k
    raise Invalid()


def list_index(k):
    """Liste için sayısal indeks ya da None (= tam-sayı olmayan metin)."""
    if isinstance(k, bool):
        raise Invalid()
    if isinstance(k, int):
        return k
    if isinstance(k, str):
        return int(k) if is_int_key(k) else None
    raise Invalid()


def get(c, k):
    if c is None:
        return None
    if isinstance(c, MapV):
        return c.d.get(key_str(k))
    if isinstance(c, list):
        i = list_index(k)
        if i is None:
            return None
        if i < 0:
            i += len(c)
        if i < 0 or i >= len(c):
            raise LookErr()
        return c[i]
    raise Invalid()  # scalar indexing (string indexing is not the subject of this test)


def set_(c, k, v):
    """Tek düzey yazma; YENİ kabı döndürür (c yerinde de değişebilir)."""
    if c is None:
        c = []
    if isinstance(c, MapV):
        c.d[key_str(k)] = v
        return c
    if isinstance(c, list):
        i = list_index(k)
        if i is None:
            m = MapV({str(j): x for j, x in enumerate(c)})
            m.d[k] = v
            return m
        if i < 0:
            i += len(c)
        if i == len(c):
            c.append(v)
            return c
        if i < 0 or i > len(c):
            raise LookErr()
        c[i] = v
        return c
    raise Invalid()


def eq(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(eq(x, y) for x, y in zip(a, b))
    if isinstance(a, MapV) and isinstance(b, MapV):
        return a.d.keys() == b.d.keys() and all(eq(v, b.d[k]) for k, v in a.d.items())
    if isinstance(a, (list, MapV)) and isinstance(b, (list, MapV)):
        return len(a.d if isinstance(a, MapV) else a) == 0 and len(b.d if isinstance(b, MapV) else b) == 0
    if isinstance(a, (list, MapV)) or isinstance(b, (list, MapV)):
        return False
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if a is None or b is None:
        return a is b
    if type(a) is not type(b):
        return False
    return a == b


def canon(v):
    """Karşılaştırılabilir kanonik biçim (bool/int karışmasın diye etiketli)."""
    if isinstance(v, list):
        return ("L", tuple(canon(x) for x in v))
    if isinstance(v, MapV):
        return ("M", tuple((k, canon(x)) for k, x in v.d.items()))
    if isinstance(v, bool):
        return ("b", v)
    if isinstance(v, int):
        return ("i", v)
    if isinstance(v, float):
        return ("i", int(v)) if v == int(v) else ("f", v)
    if isinstance(v, str):
        return ("s", v)
    if v is None:
        return ("n",)
    raise TypeError(v)


def from_json(text):
    def hook(pairs):
        return MapV(dict(pairs))
    return json.loads(text, object_pairs_hook=hook)


# ── LOOK source generation ───────────────────────────────────────────────────
def lit(v):
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        out = []
        for ch in v:
            if ch in '"\\$':
                out.append("\\" + ch)
            else:
                out.append(ch)
        return '"' + "".join(out) + '"'
    if isinstance(v, list):
        return "[" + ", ".join(lit(x) for x in v) + "]"
    if isinstance(v, MapV):
        if not v.d:
            return "array::new_assoc()"
        return "[" + ", ".join(lit(k) + " => " + lit(x) for k, x in v.d.items()) + "]"
    raise TypeError(v)


VARS = ["$a", "$b", "$c"]
STR_KEYS = ["a", "b", "k3", "name", "é", "İş", "1", "0", "2", "-1", "01", "+1", "x y"]
INT_KEYS = [0, 1, 2, 3, -1, -2, 7]
SCALARS = [0, 1, -5, 42, 9007199254740993, "", "1", "x", "é", "日本", "a\"b", True, False, None]
STRS = ["", "abc", "İstanbul", "çğüşöı", "日本語テキスト", "a😀b", "x\\y", "hello world", "éé"]


def rand_scalar(r):
    return r.choice(SCALARS)


def rand_value(r, depth=0):
    p = r.random()
    if depth < 2 and p < 0.12:
        return [rand_scalar(r) for _ in range(r.randint(0, 3))]
    if depth < 2 and p < 0.22:
        return MapV({r.choice(STR_KEYS[:5]): rand_scalar(r) for _ in range(r.randint(0, 3))})
    return rand_scalar(r)


def rand_key(r):
    return r.choice(STR_KEYS) if r.random() < 0.6 else r.choice(INT_KEYS)


# An operation = (kind, arguments). render(op) -> a LOOK line; model.apply(op) -> the expected output lines.
def gen_op(r, state):
    kinds = [
        ("init_list", 3), ("init_map", 3), ("copy", 6), ("set", 18), ("set2", 7), ("push", 8),
        ("push2", 3), ("take", 4), ("pop", 1), ("aset", 3), ("mut", 3), ("fill", 2), ("dump", 8), ("count", 3),
        ("keys", 3), ("values", 2), ("contains", 3), ("read", 6), ("eq", 5), ("json_rt", 3),
        ("foreach", 2), ("str", 6), ("bad_sort", 1), ("slice", 3),
    ]
    tot = sum(w for _, w in kinds)
    x = r.uniform(0, tot)
    for kind, w in kinds:
        x -= w
        if x <= 0:
            break
    X = r.choice(VARS)
    Y = r.choice(VARS)
    if kind == "init_list":
        return (kind, X, [rand_value(r, 1) for _ in range(r.randint(0, 5))])
    if kind == "init_map":
        d = {}
        for _ in range(r.randint(0, 5)):
            d[r.choice(STR_KEYS[:5])] = rand_value(r, 1)
        return (kind, X, MapV(d))
    if kind == "copy":
        return (kind, X, Y)
    if kind == "set":
        v = state.get(X)
        if isinstance(v, list) and r.random() < 0.3:
            k = r.choice([len(v), -len(v) if v else 0, len(v) + 1, str(len(v))])
        else:
            k = rand_key(r)
        return (kind, X, k, rand_value(r))
    if kind == "set2":
        return (kind, X, rand_key(r), rand_key(r), rand_scalar(r))
    if kind == "push":
        return (kind, X, rand_value(r))
    if kind == "push2":
        return (kind, X, rand_key(r), rand_scalar(r))
    if kind == "aset":
        return (kind, Y, X, rand_key(r), rand_scalar(r))
    if kind == "mut":
        return (kind, Y, X, rand_key(r), rand_scalar(r))
    if kind == "slice":
        return (kind, Y, X, r.randint(-4, 4), r.choice([None, r.randint(-4, 4), r.randint(-4, 4)]))
    if kind == "fill":
        return (kind, X, r.choice(["f", "g"]), r.randint(1, 30))
    if kind == "contains":
        return (kind, X, rand_scalar(r))
    if kind == "read":
        return (kind, X, rand_key(r))
    if kind == "eq":
        return (kind, X, Y)
    if kind == "str":
        s = r.choice(STRS)
        t = r.choice(STRS)
        return (kind, s, t, r.randint(0, 9), r.randint(0, 9))
    return (kind, X)


def render(op):
    k = op[0]
    J = "json::encode"
    if k in ("init_list", "init_map"):
        return f"{op[1]} = {lit(op[2])}"
    if k == "copy":
        return f"{op[1]} = {op[2]}"
    if k == "set":
        return f"try {{ {op[1]}[{lit(op[2])}] = {lit(op[3])} }} catch ($e) {{ print(\"ERR\") }}"
    if k == "set2":
        return f"try {{ {op[1]}[{lit(op[2])}][{lit(op[3])}] = {lit(op[4])} }} catch ($e) {{ print(\"ERR\") }}"
    if k == "push":
        return (f"try {{ {op[1]} = array::push({op[1]}, {lit(op[2])})\n"
                f"print(\"I \" . count({op[1]})) }} catch ($e) {{ print(\"ERR\") }}")
    if k == "push2":
        e = f"{op[1]}[{lit(op[2])}]"
        return (f"try {{ {e} = array::push({e}, {lit(op[3])})\n"
                f"print(\"I \" . count({e})) }} catch ($e) {{ print(\"ERR\") }}")
    if k == "pop":   # deprecated (warning goes to stderr); keep a few
        return f"try {{ print(\"J \" . {J}(pop({op[1]}))) }} catch ($e) {{ print(\"ERR\") }}"
    if k == "take":  # the new form of pop: last element + in-place shortening (SHRINK_PATH)
        return (f"try {{ $tk = {op[1]}[-1]\n{op[1]} = array::slice({op[1]}, 0, -1)\n"
                f"print(\"J \" . {J}($tk)) }} catch ($e) {{ print(\"ERR\") }}")
    if k == "aset":
        return f"try {{ {op[1]} = array::set({op[2]}, {lit(op[3])}, {lit(op[4])}) }} catch ($e) {{ print(\"ERR\") }}"
    if k == "slice":
        extra = "" if op[4] is None else f", {op[4]}"
        return f"try {{ {op[1]} = array::slice({op[2]}, {op[3]}{extra}) }} catch ($e) {{ print(\"ERR\") }}"
    if k == "mut":
        return f"try {{ {op[1]} = mut({op[2]}, {lit(op[3])}, {lit(op[4])}) }} catch ($e) {{ print(\"ERR\") }}"
    if k == "fill":
        return (f"for ($i = 0; $i < {op[3]}; $i++) {{ {op[1]}[\"{op[2]}\" . $i] = $i }}")
    if k == "dump":
        return f"print(\"J \" . {J}({op[1]}))"
    if k == "count":
        return f"print(\"I \" . count({op[1]}))"
    if k == "keys":
        return f"print(\"J \" . {J}(array::keys({op[1]})))"
    if k == "values":
        return f"print(\"J \" . {J}(array::values({op[1]})))"
    if k == "contains":
        return f"print(\"J \" . {J}(array::contains({op[1]}, {lit(op[2])})))"
    if k == "read":
        return f"try {{ print(\"J \" . {J}({op[1]}[{lit(op[2])}])) }} catch ($e) {{ print(\"ERR\") }}"
    if k == "eq":
        return f"print(\"J \" . {J}([{op[1]} == {op[2]}, {op[1]} != {op[2]}]))"
    if k == "json_rt":
        return (f"$t = json::decode({J}({op[1]}))\n"
                f"print(\"J \" . {J}([$t == {op[1]}, $t]))")
    if k == "foreach":
        return f"$t = []\nforeach ({op[1]} as $k => $v) {{ $t = array::push($t, [$k, $v]) }}\nprint(\"J \" . {J}($t))"
    if k == "str":
        s, t, i, n = op[1], op[2], op[3], op[4]
        return (f"$s = {lit(s)} . {lit(t)}\n"
                f"print(\"J \" . {J}([string::len($s), string::substr($s, {i}, {n}), string::substr($s, {i}), "
                f"string::reverse($s), string::index_of($s, {lit(t)}), $s, len({lit(s)})]))")
    if k == "bad_sort":
        return f"try {{ print(\"J \" . {J}(array::sort({op[1]}))) }} catch ($e) {{ print(\"ERR\") }}"
    raise ValueError(k)


PRELUDE = """use array
use json
use string
function mut($x, $k, $v) { $x[$k] = $v
    return $x }
$a = []
$b = []
$c = []
"""


def program(ops):
    return PRELUDE + "\n".join(render(o) for o in ops) + "\nprint(\"END\")\n"


# ── reference model ──────────────────────────────────────────────────────────
def ustr_index_of(s, t):
    # string::index_of returns a BYTE position (stdlib.cpp; the docs say "First position" without
    # naming the unit — len/substr/reverse use code points). The behavior of the code is modeled; noted in the report.
    i = s.find(t)
    return -1 if i < 0 else len(s[:i].encode("utf-8"))


def model_run(ops):
    """Beklenen çıktı: kanonik satır listesi. Geçersiz program -> Invalid."""
    env = {"$a": [], "$b": [], "$c": []}
    out = []

    def J(v):
        out.append(("J", canon(v)))

    def guarded(f):
        # try/catch: on error the variable stays UNCHANGED, "ERR" is printed
        saved = {k: deep(v) for k, v in env.items()}
        mark = len(out)
        try:
            f()
        except LookErr:
            env.clear()
            env.update(saved)
            del out[mark:]
            out.append(("ERR",))

    for op in ops:
        k = op[0]
        if k in ("init_list", "init_map"):
            env[op[1]] = deep(op[2])
        elif k == "copy":
            env[op[1]] = deep(env[op[2]])
        elif k == "set":
            def f(op=op):
                env[op[1]] = set_(env[op[1]], op[2], deep(op[3]))
            guarded(f)
        elif k == "set2":
            def f(op=op):
                c = env[op[1]]
                if not isinstance(c, (list, MapV)):
                    raise Invalid()
                if isinstance(c, list):
                    i = list_index(op[2])
                    if i is None:
                        raise Invalid()  # intermediate level with a text key on a list: out of scope
                    j = i + len(c) if i < 0 else i
                    if j > len(c) or j < 0:
                        raise LookErr()
                    inner = None if j == len(c) else c[j]
                else:
                    inner = get(c, op[2])
                if inner is None:
                    nk = op[3]
                    inner = MapV() if (isinstance(nk, str) and not is_int_key(nk)) else []
                elif not isinstance(inner, (list, MapV)):
                    raise Invalid()
                inner = set_(deep(inner), op[3], op[4])
                env[op[1]] = set_(c, op[2], inner)
            guarded(f)
        elif k == "push":
            def f(op=op):
                c = env[op[1]]
                if not isinstance(c, list):
                    if isinstance(c, MapV):
                        raise LookErr()
                    raise Invalid()
                c.append(deep(op[2]))
                out.append(("I", len(c)))
            guarded(f)
        elif k == "push2":
            def f(op=op):
                c = env[op[1]]
                if not isinstance(c, (list, MapV)):
                    raise Invalid()
                inner = get(c, op[2])   # out-of-range read on a list -> error
                # array::push(null / map, v) is an error: a missing key does NOT CREATE a list
                if inner is None or isinstance(inner, MapV):
                    raise LookErr()
                if not isinstance(inner, list):
                    raise Invalid()
                inner = deep(inner)
                inner.append(op[3])
                env[op[1]] = set_(c, op[2], inner)
                out.append(("I", len(inner)))
            guarded(f)
        elif k == "pop":
            def f(op=op):
                c = env[op[1]]
                if isinstance(c, MapV):
                    raise LookErr()
                if not isinstance(c, list):
                    raise Invalid()
                J(c.pop() if c else None)
            guarded(f)
        elif k == "take":
            def f(op=op):
                c = env[op[1]]
                if isinstance(c, MapV):
                    raise LookErr()   # array::slice on a map is an error (the $m[-1] read comes first, no error)
                if not isinstance(c, list):
                    raise Invalid()
                if not c:
                    raise LookErr()   # $l[-1] on an empty list is an error
                J(c.pop())
            guarded(f)
        elif k == "slice":
            # array::slice($l, offset[, length]) — PHP array_slice: a negative offset counts from the end; a negative
            # length EXCLUDES that many elements from the end; bounds are clamped. Error on a map (list-specific).
            def f(op=op):
                src = env[op[2]]
                if isinstance(src, MapV):
                    raise LookErr()
                if not isinstance(src, list):
                    raise Invalid()
                n = len(src)
                off = op[3] + n if op[3] < 0 else op[3]
                off = min(max(off, 0), n)
                ln = op[4]
                if ln is None:
                    end = n
                elif ln < 0:
                    end = max(n + ln, off)
                else:
                    end = min(off + ln, n)
                env[op[1]] = [deep(x) for x in src[off:end]]
            guarded(f)
        elif k in ("aset", "mut"):
            def f(op=op):
                src = env[op[2]]
                if not isinstance(src, (list, MapV)):
                    raise Invalid()
                if k == "aset" and isinstance(src, list):
                    # array::set on a list (extra_stdlib.cpp): an integer or integer text ("1",
                    # "01", "+1") is the same index as $l[k] = v, negative counts from the end; 0..len
                    # in place/append. Out of range and a text key turn the list into a map with the keys "0","1",…
                    # (specific to array::set only, the old behavior).
                    kk = op[3]
                    idx = None
                    if isinstance(kk, int) and not isinstance(kk, bool):
                        idx = kk
                    elif isinstance(kk, str) and re.fullmatch(r"[+-]?[0-9]+", kk):
                        idx = int(kk)
                    if idx is not None and idx < 0:
                        idx += len(src)
                    if idx is not None and 0 <= idx <= len(src):
                        env[op[1]] = set_(deep(src), idx, op[4])
                    else:
                        m = MapV({str(j): deep(x) for j, x in enumerate(src)})
                        m.d[key_str(kk)] = op[4]
                        env[op[1]] = m
                    return
                # mut(): $x[k] = v on a copy; array::set is the same on a map
                env[op[1]] = set_(deep(src), op[3], op[4])
            guarded(f)
        elif k == "fill":
            c = env[op[1]]
            if not isinstance(c, (list, MapV)):
                raise Invalid()
            for i in range(op[3]):
                c = set_(c, op[2] + str(i), i)
            env[op[1]] = c
        elif k == "dump":
            J(env[op[1]])
        elif k == "count":
            c = env[op[1]]
            if not isinstance(c, (list, MapV)):
                raise Invalid()
            out.append(("I", len(c.d) if isinstance(c, MapV) else len(c)))
        elif k == "keys":
            c = env[op[1]]
            if isinstance(c, MapV):
                J(list(c.d.keys()))
            elif isinstance(c, list):
                J(list(range(len(c))))
            else:
                raise Invalid()
        elif k == "values":
            c = env[op[1]]
            if isinstance(c, MapV):
                J(list(c.d.values()))
            elif isinstance(c, list):
                J(c)
            else:
                raise Invalid()
        elif k == "contains":
            c = env[op[1]]
            vals = list(c.d.values()) if isinstance(c, MapV) else c
            if not isinstance(vals, list):
                raise Invalid()
            J(any(eq(x, op[2]) for x in vals))
        elif k == "read":
            def f(op=op):
                c = env[op[1]]
                if not isinstance(c, (list, MapV)):
                    raise Invalid()
                J(get(c, op[2]))
            guarded(f)
        elif k == "eq":
            e = eq(env[op[1]], env[op[2]])
            J([e, not e])
        elif k == "json_rt":
            c = env[op[1]]
            if not isinstance(c, (list, MapV)):
                raise Invalid()
            J([True, c])
        elif k == "foreach":
            c = env[op[1]]
            if isinstance(c, MapV):
                J([[kk, vv] for kk, vv in c.d.items()])
            elif isinstance(c, list):
                J([[i, vv] for i, vv in enumerate(c)])
            else:
                raise Invalid()
        elif k == "str":
            s = op[1] + op[2]
            i, n = op[3], op[4]
            J([len(s), s[i:i + n], s[i:], s[::-1], ustr_index_of(s, op[2]), s, len(op[1])])
        elif k == "bad_sort":
            def f(op=op):
                c = env[op[1]]
                if isinstance(c, MapV):
                    raise LookErr()
                if not isinstance(c, list):
                    raise Invalid()
                if any(isinstance(x, (list, MapV, bool)) or x is None for x in c):
                    raise Invalid()
                if len({type(x) for x in c}) > 1:
                    raise Invalid()  # sorting of mixed types is out of scope
                J(sorted(c))
            guarded(f)
        else:
            raise ValueError(k)
    out.append(("END",))
    return out, env


def parse_output(text):
    res = []
    for line in text.splitlines():
        if line.startswith("J "):
            try:
                res.append(("J", canon(from_json(line[2:]))))
            except Exception:
                res.append(("RAW", line))
        elif line.startswith("I "):
            try:
                res.append(("I", int(line[2:])))
            except ValueError:
                res.append(("RAW", line))
        elif line in ("ERR", "END"):
            res.append((line,))
        else:
            res.append(("RAW", line))
    return res


# lk runs only with the VM (LOOK_CLI_VM=0 is now ignored); the engine list has a single element.
ENGINES = [("vm", {})]


def run_lk(lk, src, extra_env, tmpdir):
    path = os.path.join(tmpdir, "prog.lk")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    env = dict(os.environ)
    env.update(extra_env)
    try:
        p = subprocess.run([lk, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, env=env)
    except subprocess.TimeoutExpired:
        return [("RAW", "<timeout>")], "<timeout>"
    text = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    return parse_output(text), text + (("\n[stderr] " + err) if err.strip() else "") + f"\n[exit {p.returncode}]"


def diverges(lk, ops, eng_env, tmpdir):
    try:
        want = model_run(ops)[0]
    except Invalid:
        return None
    got, raw = run_lk(lk, program(ops), eng_env, tmpdir)
    return None if got == want else (want, got, raw)


def shrink(lk, ops, eng_env, tmpdir):
    cur = list(ops)
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(cur):
            cand = cur[:i] + cur[i + 1:]
            if diverges(lk, cand, eng_env, tmpdir):
                cur = cand
                changed = True
            else:
                i += 1
    return cur


def gen_program(r, length):
    """Model-geçerli bir işlem dizisi üret (geçersiz adayları atla)."""
    ops = []
    tries = 0
    while len(ops) < length and tries < length * 20:
        tries += 1
        # current variable values (the generator picks the list index based on this)
        state = model_run(ops)[1]
        op = gen_op(r, state)
        try:
            model_run(ops + [op])
        except Invalid:
            continue
        ops.append(op)
    return ops


def first_diff(want, got):
    for i in range(max(len(want), len(got))):
        w = want[i] if i < len(want) else None
        g = got[i] if i < len(got) else None
        if w != g:
            return i, w, g
    return None


def main():
    ap = argparse.ArgumentParser(description="Differential fuzz test of LOOK lists, maps and strings against a reference model.")
    ap.add_argument("lk", help="path to the lk binary")
    ap.add_argument("--iterations", type=int,
                    default=int(os.environ.get("LOOK_COLL_FUZZ_ITERATIONS", "600")),
                    help="number of random programs (env LOOK_COLL_FUZZ_ITERATIONS)")
    ap.add_argument("--seed", type=int, default=int(os.environ.get("LOOK_COLL_FUZZ_SEED", "20261118")))
    ap.add_argument("--length", type=int, default=40, help="statements per program")
    ap.add_argument("--max-failures", type=int, default=3)
    a = ap.parse_args()

    lk = os.path.abspath(a.lk)
    t0 = time.time()
    failures = 0
    stmts = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        for it in range(a.iterations):
            r = random.Random(a.seed * 1000003 + it)
            ops = gen_program(r, a.length)
            stmts += len(ops)
            for eng, eng_env in ENGINES:
                d = diverges(lk, ops, eng_env, tmpdir)
                if not d:
                    continue
                failures += 1
                small = shrink(lk, ops, eng_env, tmpdir)
                d2 = diverges(lk, small, eng_env, tmpdir) or d
                want, got, raw = d2
                fd = first_diff(want, got)
                print(f"FAIL [{eng}] program {it} (seed {a.seed}): output differs from the reference model")
                print("---- minimal program ----")
                print(program(small), end="")
                print("---- first difference (line %d) ----" % (fd[0] if fd else -1))
                print("  expected:", fd[1] if fd else None)
                print("  actual:  ", fd[2] if fd else None)
                print("---- lk output ----")
                print(raw)
                break
            if failures >= a.max_failures:
                break
    dt = time.time() - t0
    if failures:
        print(f"collections differential: {failures} FAILED program(s) ({dt:.1f} s)")
        return 1
    print(f"collections differential: all checks passed ({a.iterations} programs, {stmts} statements, "
          f"{len(ENGINES)} engines, seed {a.seed}, {dt:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

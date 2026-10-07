#!/usr/bin/env bash
# LOOK 2: setup data is SHARED with every request, not copied for it — and a request that
# writes still changes only its own copy, also when many requests run at once.
#
# Until this change a request got a deep copy of every top-level array it touched, of every
# array a route closure had captured, and of every struct default (the 1.0.3 / 1.0.4 fixes,
# needed when arrays were shared by reference). With value semantics the write itself makes
# the copy, so the copies in advance are gone: a route that only reads a 2000-element setup
# list went from 3 000 to 51 000 requests per second (cpp/bench/isolation).
# What must hold, and is checked here with 4 workers and 8 clients at once:
#   * every writing request reads back exactly what it wrote (global, nested, pushed,
#     struct default, captured list copied and written)
#   * every reading request, at any time, sees the declared values
# tests/request_isolation_test.sh proves the same for one request after another, and turns
# red for each of the three copy-on-write points when it is removed.
# Usage: shared_setup_concurrency_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7733}"
cat > "$TMP/app.lk" <<'LK'
use json
struct Cart { owner string = "nobody", items array = ["i0"], meta map = ["tags" => ["t0"]] }
$G = ["g0", "g1", "g2"]
$N = ["k" => ["n0"], "deep" => ["a" => ["b" => ["d0"]]]]
$P = ["p0"]
$BIG = []
for ($i = 0; $i < 500; $i++) { push($BIG, ["id" => $i, "tags" => ["x", "y"]]) }
$captured = $BIG
function work($u) {
    $G[1] = $u
    $N["k"][0] = $u
    $N["deep"]["a"]["b"][0] = $u
    push($P, $u)
    $BIG[7]["tags"][1] = $u
    $c = Cart { owner: $u }
    push($c.items, $u)
    $c.meta["tags"][0] = $u
    return [$G[0], $G[1], $N["k"][0], $N["deep"]["a"]["b"][0], count($P), $P[1], $BIG[7]["tags"][1], $BIG[8]["tags"][1],
            count($c.items), $c.items[1], $c.meta["tags"][0]]
}
route("GET", "/w", function() {
    $u = request::get("u")
    $mine = $captured
    $mine[3]["tags"][0] = $u
    return response::json(["w" => work($u), "mine" => $mine[3]["tags"][0], "cap" => $captured[3]["tags"][0]])
})
route("GET", "/r", function() {
    $c = Cart { }
    return response::json([$G[1], $N["k"][0], $N["deep"]["a"]["b"][0], count($P), $BIG[7]["tags"][1], $captured[3]["tags"][0],
                           count($c.items), $c.meta["tags"][0]])
})
LK
cat > "$TMP/load.py" <<'PY'
import sys, json, threading, urllib.request
port, bad, done = int(sys.argv[1]), [], [0]
READ = ["g1", "n0", "d0", 1, "y", "x", 1, "t0"]
def get(p): return json.loads(urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, p), timeout=10).read().decode())
def writer(t):
    for n in range(150):
        u = "T%d-%d" % (t, n)
        try:
            r = get("/w?u=" + u)
            want = {"w": ["g0", u, u, u, 2, u, u, "y", 2, u, u], "mine": u, "cap": "x"}
            if r != want: bad.append(("w", u, r))
        except Exception as e: bad.append(("w-error", u, str(e)))
        done[0] += 1
def reader(t):
    for n in range(150):
        try:
            r = get("/r")
            if r != READ: bad.append(("r", t, r))
        except Exception as e: bad.append(("r-error", t, str(e)))
        done[0] += 1
ts = [threading.Thread(target=writer, args=(i,)) for i in range(5)] + [threading.Thread(target=reader, args=(i,)) for i in range(3)]
[t.start() for t in ts]; [t.join() for t in ts]
print("requests=%d bad=%d %s" % (done[0], len(bad), json.dumps(bad[:2])))
PY
fail=0
for mode in "LOOK_VM_STRICT=1" "LOOK_BYTECODE=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
    out="$(python3 "$TMP/load.py" "$PORT" 2>&1 | tail -1)"
    alive="$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/r")"
    errs="$(grep -c 'Dispatch error\|VM BUG\|runs on the interpreter' "$TMP/log.txt")"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "${out%% \[*}" = "requests=1200 bad=0" ] && [ "$alive" = "200" ] && [ "$errs" = "0" ]; then
        echo "  OK   [$mode] 1200 concurrent requests: every writer read its own writes, every reader the declared values"
    else echo "  FAIL [$mode] $out alive=$alive log-errors=$errs $(grep -m1 'Dispatch error' "$TMP/log.txt" | cut -c1-140)"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: setup data is shared between concurrent requests without leaking writes" || { echo "FAIL: shared setup data"; exit 1; }

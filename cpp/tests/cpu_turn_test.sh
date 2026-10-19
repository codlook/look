#!/usr/bin/env bash
# Long CPU work in one request does not hold the other requests of its thread.
#
# With fibers (the default dispatch on Linux since 1.0.20) one thread serves many requests and
# switches between them only when one gives up its turn. Up to 1.0.24 LOOK code never did: a
# 0.45 s loop made a plain request on the same thread wait 0.36 s on the VM and 32 s on the
# interpreter (the loop is slower there), and password hashing (auth::hash / auth::verify, a C++
# built-in) held it for its whole run. Now LOOK code gives up its turn about every millisecond
# (backward jumps and calls on the VM, loop turns and calls on the interpreter) and password
# hashing runs on a separate pool of threads while the request waits.
# While one long request runs, 40 plain requests must each be answered within 0.3 s.
# Usage: cpu_turn_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7826}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
cat > "$TMP/app.lk" <<'LK'
route("GET", "/fast", function() { return response::text("ok") })
route("GET", "/loop/{n}", function($n) {
    $m = int($n)
    $s = 0
    for ($i = 0; $i < $m; $i = $i + 1) { $s = $s + 1 }
    return response::text("s=" . $s)
})
function deep($k) { if ($k == 0) { return 0 } return 1 + deep($k - 1) }
route("GET", "/calls/{n}", function($n) {
    $m = int($n)
    $s = 0
    $j = 0
    while ($j < $m) { $s = $s + deep(50)
        $j = $j + 1 }
    return response::text("s=" . $s)
})
route("GET", "/hash", function() {
    $h = auth::hash("secret", 400000)
    return response::text((auth::verify("secret", $h) ? "ok" : "BAD") . (auth::verify("wrong", $h) ? "BAD" : "ok"))
})
LK
cat > "$TMP/check.py" <<'PY'
import sys, threading, time, urllib.request
port, path, want = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def get(p):
    t = time.time()
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, p), timeout=120) as r:
        return time.time() - t, r.read().decode()
res = []
th = threading.Thread(target=lambda: res.append(get(path))); th.start(); time.sleep(0.05)
lat = []
for _ in range(40):
    lat.append(get("/fast")[0]); time.sleep(0.02)
th.join()
print("slow=%.2f body=%s max=%.3f" % (res[0][0], res[0][1] if res[0][1] in (want,) else "WRONG:" + res[0][1], max(lat)))
PY
fail=0
for mode in "LOOK_X=1" "LOOK_FIBER_DISPATCH=0"; do
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/fast" && break; sleep 0.1; done

    n=50000000; c=500000
    for case in "loop:/loop/$n:s=$n" "calls:/calls/$c:s=$((c * 50))" "hash:/hash:okok"; do
        name="${case%%:*}"; rest="${case#*:}"; path="${rest%%:*}"; want="${rest#*:}"
        out="$(cd "$TMP" && python3 check.py "$PORT" "$path" "$want" 2>&1 | tail -1)"
        slow="$(echo "$out" | sed -n 's/^slow=\([0-9.]*\).*/\1/p')"; max="$(echo "$out" | sed -n 's/.* max=\([0-9.]*\).*/\1/p')"
        if [ -z "$slow" ] || ! python3 -c "import sys; sys.exit(0 if $slow >= 0.2 else 1)"; then
            echo "  FAIL [$mode] $name: did not run long enough ($out) — the test did not reach its case"; fail=1
        elif echo "$out" | grep -q 'body=WRONG'; then
            echo "  FAIL [$mode] $name: wrong result ($out)"; fail=1
        elif python3 -c "import sys; sys.exit(0 if $max < 0.3 else 1)"; then
            echo "  OK   [$mode] $name ran ${slow} s; a plain request waited at most ${max} s"
        else
            echo "  FAIL [$mode] $name ran ${slow} s and a plain request waited ${max} s"; fail=1
        fi
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
done
[ $fail = 0 ] && echo "PASS: long CPU work does not hold other requests" || { echo "FAIL: long CPU work held other requests"; exit 1; }

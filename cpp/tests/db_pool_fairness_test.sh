#!/usr/bin/env bash
# Requests waiting for a pooled database connection are served in the order they started waiting.
#
# When a connection came back, the pool put it on the free list and woke the oldest waiter; before
# that fiber ran, a newly arriving request found the free connection and took it, and the woken
# fiber went back to the END of the queue. Under load the same request lost its turn again and
# again: measured on MySQL at c=1000, p99 270 ms. Now a returned connection goes straight to the
# oldest waiter (p99 52 ms there).
# Here: a pool of one connection, a query of ~2 ms, 48 clients sending requests one after another.
# The test counts, for each request, how many other requests were answered while it waited
# ("overtaken"). In a fair queue that is the number of other clients, 47, however fast or busy the
# machine is. Measured: now p99 47-48 and at most 51, with and without other load on the CPU;
# before the fix p99 ~139 and at most 185-273. Limits: p99 within 1.5x and the most within 2x of 47.
# (The test used to compare latencies — p99 within 1.5x of the median. On the same build that ratio
# swung between 1.1x and 2x with the load of the machine, and the test went red with no regression.)
# Usage: db_pool_fairness_test.sh <lk-fcgi>
set -u
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; pid=""
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}" DBF="$T/f.db"
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
command -v python3 >/dev/null || { echo "FAIL: python3 is needed"; exit 1; }
cat > "$T/app.lk" <<'LK'
use db
$c = db::connect("sqlite://" . env("DBF"))
route("GET", "/q", function() use ($c) {
    $r = db::query($c, "WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 20000) SELECT count(*) AS n FROM s", [])
    return response::text("" . $r[0]["n"])
})
route("GET", "/ok", fn() => response::text("up"))
LK
( cd "$T" && exec env LOOK_DB_POOL_SIZE=1 "$FCGI" --mode http --port 7764 app.lk > "$T/srv.log" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -m 1 -o /dev/null localhost:7764/ok && break; sleep 0.1; done
cat > "$T/load.py" <<'PY'
import http.client, threading, time, statistics
N = 48
lat, over, bad, done, lock = [], [], [0], [0], threading.Lock()
def client():
    c = http.client.HTTPConnection("127.0.0.1", 7764, timeout=60)
    for _ in range(25):
        with lock: before = done[0]
        t = time.time(); c.request("GET", "/q"); r = c.getresponse(); body = r.read(); d = time.time() - t
        with lock:
            over.append(done[0] - before); done[0] += 1   # answered while this one waited
            lat.append(d)
            if body != b"20000": bad[0] += 1
ts = [threading.Thread(target=client) for _ in range(N)]
[t.start() for t in ts]; [t.join() for t in ts]
lat.sort(); over.sort(); med = statistics.median(lat)
print("%d %d %d %d %d %.1f %.1f" % (len(lat), bad[0], N - 1, over[int(len(over) * 0.99)], over[-1],
                                    med * 1000, lat[int(len(lat) * 0.99)] * 1000))
PY
read -r n bad others op99 omax med p99 < <(python3 "$T/load.py")
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
echo "  requests=$n wrong=$bad overtaken: p99=$op99 max=$omax (fair: $others)  latency: median=${med}ms p99=${p99}ms"
fail=0
[ "$n" = 1200 ] && [ "$bad" = 0 ] && echo "  OK   1200 requests, all answered correctly" || { echo "  FAIL requests: $n, wrong: $bad"; fail=1; }
if awk "BEGIN{exit !($op99 <= 1.5 * $others && $omax <= 2 * $others)}"; then
    echo "  OK   overtaken p99 within 1.5x and the most within 2x of $others: waiting requests are served in order"
else echo "  FAIL overtaken p99 $op99 / most $omax against $others other clients: some requests lose their turn"; fail=1; fi
[ $fail = 0 ] && echo "PASS: pool waiters are served in order" || echo "FAIL: pool fairness"
exit $fail

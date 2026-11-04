#!/usr/bin/env bash
# Requests waiting for a pooled database connection are served in the order they started waiting.
#
# When a connection came back, the pool put it on the free list and woke the oldest waiter; before
# that fiber ran, a newly arriving request found the free connection and took it, and the woken
# fiber went back to the END of the queue. Under load the same request lost its turn again and
# again: measured on MySQL at c=1000, p99 270 ms. Now a returned connection goes straight to the
# oldest waiter (p99 52 ms there).
# Here: a pool of one connection, a query of ~2 ms, 48 clients sending requests one after another.
# With a fair queue every request waits about as long as the others: p99 within 1.5x and the
# slowest within 2x the median (measured ~1.1x; before, p99 2-5x and the slowest 3-5x).
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
lat, bad, lock = [], [0], threading.Lock()
def client():
    c = http.client.HTTPConnection("127.0.0.1", 7764, timeout=60)
    for _ in range(25):
        t = time.time(); c.request("GET", "/q"); r = c.getresponse(); body = r.read(); d = time.time() - t
        with lock:
            lat.append(d)
            if body != b"20000": bad[0] += 1
ts = [threading.Thread(target=client) for _ in range(48)]
[t.start() for t in ts]; [t.join() for t in ts]
lat.sort(); med = statistics.median(lat); p99 = lat[int(len(lat) * 0.99)]
print("%d %d %.1f %.1f %.1f" % (len(lat), bad[0], med * 1000, p99 * 1000, lat[-1] * 1000))
PY
read -r n bad med p99 mx < <(python3 "$T/load.py")
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
echo "  requests=$n wrong=$bad median=${med}ms p99=${p99}ms max=${mx}ms"
fail=0
[ "$n" = 1200 ] && [ "$bad" = 0 ] && echo "  OK   1200 requests, all answered correctly" || { echo "  FAIL requests: $n, wrong: $bad"; fail=1; }
# Ölçüm: adil kuyrukta p99 ve en yavaş istek medyanın ~1.1 katı; eski davranışta (sıra kaybı) p99 2 kat, en yavaş 3-4 kat.
if awk "BEGIN{exit !($p99 <= 1.5 * $med && $mx <= 2 * $med)}"; then echo "  OK   p99 within 1.5x and the slowest within 2x the median: waiting requests are served in order"
else echo "  FAIL p99 ${p99}ms / slowest ${mx}ms against a median of ${med}ms: some requests lose their turn"; fail=1; fi
[ $fail = 0 ] && echo "PASS: pool waiters are served in order" || echo "FAIL: pool fairness"
exit $fail

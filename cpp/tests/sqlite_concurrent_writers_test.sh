#!/usr/bin/env bash
# Concurrent writers to one SQLite database are not refused with "database is locked".
#
# SQLite lets one connection write at a time; the others sleep and retry within the busy
# timeout, and that retry is not fair. Up to 1.0.20, with 20 concurrent writers some requests
# kept losing their turn, waited past the timeout and got a 500 "database is locked" (code 5):
# 27 in 60 s (busy timeout 2 s), still 3 with 5 s, with waits up to 10 s. Writers of this
# process now queue for one database in arrival order; reads do not queue.
# This test runs plain writes, explicit transactions (commit and rollback) and reads at once and
# checks that no request failed and that the database holds exactly what was committed.
# Usage: sqlite_concurrent_writers_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7821}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
cat > "$TMP/app.lk" <<'LK'
function conn() { return db::connect("sqlite://w.db") }
$c0 = conn()
db::exec($c0, "CREATE TABLE IF NOT EXISTS hits (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT)")
route("GET", "/w", function() {
    $c = conn()
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["w"])
    return response::text("ok")
})
route("GET", "/tx", function() {
    $c = conn()
    db::begin($c)
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["tx"])
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["tx"])
    db::commit($c)
    return response::text("ok")
})
route("GET", "/rb", function() {
    $c = conn()
    db::begin($c)
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["rolled-back"])
    db::rollback($c)
    return response::text("ok")
})
route("GET", "/r", function() {
    $r = db::query(conn(), "SELECT COUNT(*) AS c FROM hits")
    return response::text("n=" . $r[0]["c"])
})
route("GET", "/count", function() {
    $r = db::query(conn(), "SELECT kind, COUNT(*) AS c FROM hits GROUP BY kind ORDER BY kind")
    $out = ""
    foreach ($r as $row) { $out = $out . $row["kind"] . "=" . $row["c"] . " " }
    return response::text($out)
})
LK
cat > "$TMP/load.py" <<'PY'
import socket, sys, threading, collections
port = int(sys.argv[1]); codes = collections.Counter(); lock = threading.Lock()
def connect():
    s = socket.create_connection(("127.0.0.1", port), timeout=30); return s
def get(s, path):
    # keep-alive: one connection per client, as a browser or a load balancer does
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
    d = b""
    while b"\r\n\r\n" not in d:
        x = s.recv(4096)
        if not x: raise OSError("closed")
        d += x
    head, body = d.split(b"\r\n\r\n", 1)
    n = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1])
    while len(body) < n:
        x = s.recv(4096)
        if not x: raise OSError("closed")
        body += x
    return head.split(b" ")[1].decode()
def worker(path, n):
    s = connect()
    for i in range(n):
        try: c = get(s, path)
        except Exception:
            c = "error"
            try: s.close()
            except Exception: pass
            s = connect()
        with lock: codes[(path, c)] += 1
ts = [threading.Thread(target=worker, args=(p, 300)) for p in ["/w"] * 20 + ["/tx"] * 5 + ["/rb"] * 3 + ["/r"] * 4]
for t in ts: t.start()
for t in ts: t.join()
bad = {k: v for k, v in codes.items() if k[1] != "200"}
print("failed=%s" % (sum(bad.values())), bad if bad else "")
PY
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 8 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/r" && break; sleep 0.1; done
out="$(python3 "$TMP/load.py" "$PORT" 2>&1 | tail -1)"
count="$(curl -s -m 10 "localhost:$PORT/count")"
locked="$(grep -c 'database is locked' "$TMP/log.txt")"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
fail=0
if [ "$out" = "failed=0 " ]; then echo "  OK   9600 requests (20 writers, 5 transactions, 3 rollbacks, 4 readers at once): none failed"
else echo "  FAIL requests failed: $out (log: $locked 'database is locked')"; fail=1; fi
if [ "$count" = "tx=3000 w=6000 " ]; then echo "  OK   the database holds exactly the committed rows (6000 writes, 3000 from transactions, no rolled-back row)"
else echo "  FAIL database contents: [$count] (want tx=3000 w=6000)"; fail=1; fi
[ $fail = 0 ] && echo "PASS: concurrent SQLite writers" || { echo "FAIL: concurrent SQLite writers"; exit 1; }

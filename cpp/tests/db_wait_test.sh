#!/usr/bin/env bash
# A database query that waits does not hold other requests.
#
# 1.0.20 made fibers the default: one thread serves many requests and switches between them while
# one waits. MySQL queries switched; PostgreSQL and SQLite queries held the whole thread, so one
# slow query delayed the other requests of that thread (measured on 1.0.21: one 1 s query, about
# one request in 20 waited up to 1 s; with LOOK_FIBER_DISPATCH=0 none did). Separately, every
# request took a connection from every database pool when it started, so with the pool in use a
# route that never touches the database waited for a connection too (both dispatch models).
#
# Here another process holds the SQLite database locked for 1.5 s, so writes wait without using
# the CPU. While they wait, requests to a plain route must be answered at once:
#   1. one waiting write;   2. more waiting writes than the pool has connections;
#   and while one long CPU-bound SQLite statement runs (WITH RECURSIVE, about 1 s).
# Then: 3. many more concurrent transactions than connections all finish with exactly the
# committed rows (no deadlock); 4. every query of a transaction stays on one connection (a
# connection-private temporary table holds the request's own value); 5. a database server that
# never answers ends the query after its timeout instead of holding the request forever.
# Optional: PG_DSN=postgres://... and MY_DSN=mysql[s]://... repeat case 1 on a real server with
# pg_sleep(1) / SLEEP(1) (mysqls:// covers the TLS read path).
# Usage: db_wait_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7823}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
cat > "$TMP/app.lk" <<'LK'
function conn() { return db::connect("sqlite://w.db") }
$c0 = conn()
db::exec($c0, "CREATE TABLE IF NOT EXISTS hits (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT)")
route("GET", "/fast", function() {
    return response::text("ok")
})
route("GET", "/w", function() {
    db::exec(conn(), "INSERT INTO hits (kind) VALUES (?)", ["w"])
    return response::text("ok")
})
route("GET", "/tx", function() {
    $c = conn()
    db::begin($c)
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["tx"])
    db::query($c, "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM r WHERE n < 20000) SELECT count(*) AS c FROM r")
    db::exec($c, "INSERT INTO hits (kind) VALUES (?)", ["tx"])
    db::commit($c)
    return response::text("ok")
})
route("GET", "/mine/{id}", function($id) {
    $c = conn()
    db::begin($c)
    db::exec($c, "CREATE TEMP TABLE IF NOT EXISTS mine (v TEXT)")
    db::exec($c, "DELETE FROM mine")
    db::exec($c, "INSERT INTO mine (v) VALUES (?)", [$id])
    db::query($c, "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM r WHERE n < 5000) SELECT count(*) AS c FROM r")
    $r = db::query($c, "SELECT v FROM mine")
    db::commit($c)
    return response::text($r[0]["v"])
})
route("GET", "/pg", function() {
    db::query(db::connect(env("PG_DSN", "")), "SELECT pg_sleep(1)")
    return response::text("pg")
})
route("GET", "/my", function() {
    db::query(db::connect(env("MY_DSN", "")), "SELECT SLEEP(1)")
    return response::text("my")
})
route("GET", "/silent", function() {
    $c = db::connect("mysql://u:p@127.0.0.1:" . env("SILENT_PORT", "1") . "/d", ["query_timeout" => 1000])
    db::query($c, "SELECT 1")
    return response::text("answered")
})
route("GET", "/cpu", function() {
    $r = db::query(conn(), "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM t WHERE n < 6000000) SELECT COUNT(*) AS c FROM t")
    return response::text("done")
})
route("GET", "/count", function() {
    $r = db::query(conn(), "SELECT kind, COUNT(*) AS c FROM hits GROUP BY kind ORDER BY kind")
    $out = ""
    foreach ($r as $row) { $out = $out . $row["kind"] . "=" . $row["c"] . " " }
    return response::text($out)
})
LK
cat > "$TMP/check.py" <<'PY'
import sys, threading, time, sqlite3, urllib.request, collections
port, case = int(sys.argv[1]), sys.argv[2]
def get(path):
    t = time.time()
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=60) as r:
            return r.status, r.read().decode(), time.time() - t
    except urllib.error.HTTPError as e:
        return e.code, "", time.time() - t
    except Exception as e:
        return "error", str(e), time.time() - t
def fast_while(slow_paths, hold_lock):
    # Another process holds the write lock; the slow requests wait for it without using the CPU.
    holder = None
    if hold_lock:
        lk = sqlite3.connect("w.db", timeout=10, isolation_level=None, check_same_thread=False)
        lk.execute("BEGIN EXCLUSIVE")
        def release():
            time.sleep(1.5); lk.execute("COMMIT"); lk.close()
        holder = threading.Thread(target=release); holder.start()
    res = []
    ts = [threading.Thread(target=lambda p=p: res.append(get(p))) for p in slow_paths]
    for t in ts: t.start()
    time.sleep(0.15)
    lat = []
    for _ in range(40):
        lat.append(get("/fast")[2]); time.sleep(0.02)
    for t in ts: t.join()
    if holder: holder.join()
    slow_ok = sum(1 for r in res if r[0] == 200)
    print("max=%.3f slow=%.2f ok=%d/%d" % (max(lat), max(r[2] for r in res), slow_ok, len(res)))
if case == "one":   fast_while(["/w"], True)
if case == "many":  fast_while(["/w"] * int(__import__("os").environ["MANY"]), True)
if case == "cpu":   fast_while(["/cpu"], False)
if case == "pg":    fast_while(["/pg"], False)
if case == "my":    fast_while(["/my"], False)
if case == "tx":
    codes = collections.Counter(); lock = threading.Lock()
    def worker():
        for _ in range(15):
            c = get("/tx")[0]
            with lock: codes[c] += 1
    ts = [threading.Thread(target=worker) for _ in range(40)]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    print("codes=%s secs=%.1f" % (dict(codes), time.time() - t0))
if case == "mine":
    bad = []; lock = threading.Lock()
    def worker(k):
        for i in range(20):
            want = "%d-%d" % (k, i)
            st, body, _ = get("/mine/" + want)
            if st != 200 or body != want:
                with lock: bad.append((want, st, body))
    ts = [threading.Thread(target=worker, args=(k,)) for k in range(32)]
    for t in ts: t.start()
    for t in ts: t.join()
    print("bad=%d %s" % (len(bad), bad[:3]))
PY
fail=0
SILENT_PORT=$((PORT + 1)); export SILENT_PORT
python3 -c "
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $SILENT_PORT)); s.listen(64); held = []
while True:
    c, _ = s.accept(); held.append(c)
" & silent_pid=$!
trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null; kill $silent_pid 2>/dev/null' EXIT
run() {   # $1 = dispatch setting
    # More waiting writes than the SQLite pool's 4 connections. With fibers 4 workers are enough.
    # With LOOK_FIBER_DISPATCH=0 a request holds its worker until it ends, so 8 workers and 6
    # writes: 4 hold the pool, 2 wait for it, 2 workers stay free for the plain route.
    if [ "$1" = LOOK_FIBER_DISPATCH=0 ]; then WORKERS=8; MANY=6; else WORKERS=4; MANY=8; fi
    export MANY
    rm -f "$TMP"/w.db*
    ( cd "$TMP" && exec env $1 PG_DSN="${PG_DSN:-}" MY_DSN="${MY_DSN:-}" "$FCGI" --mode http --port "$PORT" --workers $WORKERS app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/fast" && break; sleep 0.1; done
    cd "$TMP"
    for c in one many; do
        out="$(python3 check.py "$PORT" "$c" 2>&1 | tail -1)"
        max="$(echo "$out" | sed -n 's/^max=\([0-9.]*\).*/\1/p')"
        slow="$(echo "$out" | sed -n 's/.* slow=\([0-9.]*\).*/\1/p')"
        okn="$(echo "$out" | sed -n 's/.* ok=\(.*\)$/\1/p')"
        if [ $c = one ]; then want="1/1"; label="one waiting write"
        else want="$MANY/$MANY"; label="$MANY waiting writes, pool of 4, $WORKERS workers"; fi
        # the case is only reached when the writes really waited for the lock
        if [ -z "$slow" ] || ! python3 -c "import sys; sys.exit(0 if $slow >= 1.0 else 1)"; then
            echo "  FAIL [${1:-default}] $label: the writes did not wait for the lock ($out) — the test did not reach its case"; fail=1
        elif [ "$okn" != "$want" ]; then
            echo "  FAIL [${1:-default}] $label: writes failed ($out)"; fail=1
        elif python3 -c "import sys; sys.exit(0 if $max < 0.3 else 1)"; then
            echo "  OK   [${1:-default}] $label: plain route answered in at most ${max} s (writes waited ${slow} s)"
        else
            echo "  FAIL [${1:-default}] $label: a plain route waited ${max} s while database writes waited ($out)"; fail=1
        fi
    done
    # A long CPU-bound SQLite statement (no lock, no server): it gives the other requests a turn
    # every 2 ms. In 1.0.22 the turn was given but the scheduler only looked for new connections
    # once nothing was ready, so a plain request still waited ~0.8 s.
    out="$(python3 check.py "$PORT" cpu 2>&1 | tail -1)"
    max="$(echo "$out" | sed -n 's/^max=\([0-9.]*\).*/\1/p')"
    slow="$(echo "$out" | sed -n 's/.* slow=\([0-9.]*\).*/\1/p')"
    if [ -z "$slow" ] || ! python3 -c "import sys; sys.exit(0 if $slow >= 0.3 else 1)"; then
        echo "  FAIL [${1:-default}] long SQLite statement: it did not run long enough ($out) — the test did not reach its case"; fail=1
    elif echo "$out" | grep -q 'ok=1/1' && python3 -c "import sys; sys.exit(0 if $max < 0.3 else 1)"; then
        echo "  OK   [${1:-default}] long CPU-bound SQLite statement: plain route answered in at most ${max} s (statement took ${slow} s)"
    else echo "  FAIL [${1:-default}] long CPU-bound SQLite statement: a plain route waited ${max} s ($out)"; fail=1; fi
    for ext in pg my; do
        if [ $ext = pg ]; then dsn="${PG_DSN:-}"; label="one pg_sleep(1) on PostgreSQL"
        else dsn="${MY_DSN:-}"; label="one SLEEP(1) on MySQL"
             case "$dsn" in mysqls://*|mariadbs://*|*tls=*|*ssl=*) label="$label over TLS";; esac; fi
        [ -n "$dsn" ] || continue
        out="$(python3 check.py "$PORT" $ext 2>&1 | tail -1)"
        max="$(echo "$out" | sed -n 's/^max=\([0-9.]*\).*/\1/p')"
        slow="$(echo "$out" | sed -n 's/.* slow=\([0-9.]*\).*/\1/p')"
        if [ -n "$max" ] && echo "$out" | grep -q 'ok=1/1' && python3 -c "import sys; sys.exit(0 if $slow >= 0.9 and $max < 0.3 else 1)"; then
            echo "  OK   [${1:-default}] $label: plain route answered in at most ${max} s (query took ${slow} s)"
        else echo "  FAIL [${1:-default}] $label: $out"; fail=1; fi
    done
    curl -s -o /dev/null -m 5 "localhost:$PORT/count"   # warm
    out="$(timeout 120 python3 check.py "$PORT" tx 2>&1 | tail -1)"
    count="$(curl -s -m 10 "localhost:$PORT/count")"
    want="tx=1200 w=$((MANY + 1)) "
    if echo "$out" | grep -q "^codes={200: 600}" && [ "$count" = "$want" ]; then
        echo "  OK   [${1:-default}] 600 transactions from 40 clients on a pool of 4 all finished, rows exact (${out##* })"
    else echo "  FAIL [${1:-default}] transactions: $out, rows [$count] (want $want)"; fail=1; fi
    # A database server that accepts the connection and never answers: the query ends with an
    # error after query_timeout (1 s here). Up to 1.0.21 the fiber wait had no limit — the
    # request waited forever (the worker model gave up after the timeout).
    t0=$(date +%s%N)
    code="$(curl -s -m 15 -o /dev/null -w '%{http_code}' "localhost:$PORT/silent")"
    ms=$(( ($(date +%s%N) - t0) / 1000000 ))
    if [ "$code" = 500 ] && [ $ms -lt 5000 ] && grep -q 'query timeout' "$TMP/log.txt"; then
        echo "  OK   [${1:-default}] silent database server: query timed out after ${ms} ms"
    else echo "  FAIL [${1:-default}] silent database server: HTTP $code after ${ms} ms (want 500 after about 1 s)"; fail=1; fi
    out="$(timeout 120 python3 check.py "$PORT" mine 2>&1 | tail -1)"
    if [ "$out" = "bad=0 []" ]; then echo "  OK   [${1:-default}] 640 transactions each kept one connection (temporary table held the request's own value)"
    else echo "  FAIL [${1:-default}] connection inside a transaction: $out"; fail=1; fi
    cd - >/dev/null
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
}
run ""   # default (fibers)
run LOOK_FIBER_DISPATCH=0
[ $fail = 0 ] && echo "PASS: a waiting database query does not hold other requests" || { echo "FAIL: a waiting database query held other requests"; exit 1; }

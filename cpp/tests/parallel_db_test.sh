#!/bin/bash
# parallel() + DB regression test — connection leak and pool pressure.
#
# The leak class is insidious: the first 1-2 requests WORK, then the pool is exhausted and the endpoint
# hangs forever (NO error in the log). So testing with a single request is not enough —
# sending MORE requests than the pool size is a must.
#
# Root cause (fixed): parallel() creates a raw std::thread; a db::query inside the task
# takes a LAZY conn into this thread's thread_local via get_conn (unlike a request thread,
# acquire_thread_connections() is not called). If it is not released when the task ends the conn does NOT
# return to the pool → a permanent leak. Fix: an RAII DbGuard in the task lambda →
# release_thread_connections() (both VM/PARALLEL_CALL and interpreter parallel()).
#
# Usage: bash parallel_db_test.sh [lk-fcgi_path]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
FCGI="${1:-$DIR/../build/lk-fcgi}"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"   # absolute: we will cd into $TMP
TMP=$(mktemp -d)
PORT=9671
trap 'rm -rf "$TMP"' EXIT

# The DB path is RELATIVE + the server runs inside $TMP: an absolute MSYS path ("/tmp/...") cannot be opened by the Windows
# binary (fine on Linux). A relative path works on both platforms.
cat > "$TMP/app.lk" <<'LK'
$db = db::connect("sqlite://t.db")
db::exec($db, "CREATE TABLE IF NOT EXISTS t (v TEXT)")
db::exec($db, "DELETE FROM t")
db::exec($db, "INSERT INTO t VALUES ('x')")
route("GET","/par", function(){
  $ch = channel(1)
  parallel(function(){ send($ch, count(db::query($db, "SELECT v FROM t"))) })
  return response::text("par=" . receive($ch))
})
route("GET","/par8", function(){
  $ch = channel(8)
  $i = 0
  while ($i < 8) { parallel(function(){ send($ch, count(db::query($db, "SELECT v FROM t"))) }); $i = $i + 1 }
  $sum = 0; $j = 0
  while ($j < 8) { $sum = $sum + receive($ch); $j = $j + 1 }
  return response::text("sum=" . $sum)
})
LK

cd "$TMP"   # so the relative "t.db" lands here; $srv should be the real fcgi PID (not a subshell)

fail=0
rm -f t.db
"$FCGI" --mode http --port $PORT app.lk >/dev/null 2>&1 &
srv=$!
sleep 2

# 1) Leak: more consecutive requests than the pool size (default 4)
ok=0
for i in $(seq 1 10); do
  [ "$(curl -s --max-time 6 "http://127.0.0.1:$PORT/par")" = "par=1" ] && ok=$((ok+1))
done
# 2) Pool pressure: 8 concurrent tasks / 4 conns
sum=$(curl -s --max-time 18 "http://127.0.0.1:$PORT/par8")

kill $srv 2>/dev/null || true
wait $srv 2>/dev/null || true
sleep 1

if [ "$ok" = "10" ] && [ "$sum" = "sum=8" ]; then
  echo "PASS: sızıntı yok (10/10), havuz baskısı OK ($sum)"
else
  echo "FAIL: ardışık=$ok/10 (sızıntı!), 8-paralel=[$sum] (beklenen sum=8)"
  fail=1
fi
exit $fail

#!/usr/bin/env bash
# Default workers follow the CPUs LOOK may use; the database pool opens connections as needed.
#
# 1.0.x started CPUs × 4 worker threads (at most 64) everywhere and opened the whole database
# pool — one connection per worker — at db::connect. With fiber dispatch (Linux --mode http) a
# worker no longer waits on the database, so the extra threads only cost memory and tail latency
# (measured: same throughput at 8 and 32 workers, p99 16 ms vs 34 ms). The CPU count also ignored
# the process's CPU affinity (taskset, systemd CPUAffinity=).
#
# Now: fiber dispatch → workers = usable CPUs (affinity and cgroup quota); a blocking model
# (FastCGI, LOOK_FIBER_DISPATCH=0, Windows) keeps CPUs × 4 (≤ 64), where a thread does wait.
# The pool is independent of the workers: db::connect opens one connection (a wrong password
# still stops the start), the rest open when requests need them, up to LOOK_DB_POOL_SIZE
# (default 4 × CPUs, 8..64; SQLite 4). --workers / LOOK_WORKERS / LOOK_DB_POOL_SIZE are applied as given.
# Usage: worker_pool_defaults_test.sh <lk-fcgi>   (needs curl, taskset)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7721; FP=7722
T=$(mktemp -d); pid=""
trap '{ kill $pid; wait $pid; } 2>/dev/null; rm -rf "$T"' EXIT
fail=0
is() { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }
command -v taskset >/dev/null || { echo "FAIL: taskset is needed"; exit 1; }
NCPU=$(nproc); [ "$NCPU" -ge 2 ] || { echo "FAIL: needs at least 2 CPUs (has $NCPU)"; exit 1; }
ALL="0-$((NCPU - 1))"

cat > "$T/app.lk" <<'EOF'
use runtime
$db = db::connect("sqlite://" . env("DBF"))
route("GET", "/", fn() => response::text("up"))
route("GET", "/slow", function() use ($db) {
    $r = db::query($db, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c LIMIT 1500000) SELECT count(*) AS n FROM c", [])
    return response::text("" . $r[0]["n"])
})
route("GET", "/pool", fn() => response::text("" . runtime::stats()["db_pool_size"]))
EOF
start() {   # start <taskset cpus> <log> [env...] -- [args...]
    local cpus="$1" log="$2"; shift 2
    { kill $pid; wait $pid; } 2>/dev/null
    local envs=(); while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done; [ "${1:-}" = -- ] && shift
    ( cd "$T" && exec taskset -c "$cpus" env -i PATH=/usr/bin:/bin DBF="$T/db.sqlite" "${envs[@]}" "$LKF" "$@" app.lk >"$T/$log" 2>&1 ) & pid=$!
    local i; for i in $(seq 1 50); do grep -qE "workers=[0-9]+" "$T/$log" 2>/dev/null && return 0
        kill -0 $pid 2>/dev/null || return 1; sleep 0.2; done; return 1
}
workers_in() { grep -oE "workers=[0-9]+" "$T/$1" | head -1 | cut -d= -f2; }
waitup() { local i; for i in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$P/" && return 0; sleep 0.2; done; return 1; }

echo "== workers"
start 0,1 h1.log -- --mode http --port $P || { echo "FAIL: did not start"; cat "$T/h1.log"; exit 1; }
is "--mode http (fiber) on 2 allowed CPUs: workers = 2" "$(workers_in h1.log)" 2
start 0,1 h2.log LOOK_WORKERS=5 -- --mode http --port $P
is "LOOK_WORKERS=5 is applied as given"                   "$(workers_in h2.log)" 5
start 0,1 h3.log -- --mode http --port $P --workers 3
is "--workers 3 is applied as given"                      "$(workers_in h3.log)" 3
start 0,1 h4.log LOOK_FIBER_DISPATCH=0 -- --mode http --port $P
is "LOOK_FIBER_DISPATCH=0 (a thread per connection): 2 CPUs × 4" "$(workers_in h4.log)" 8
start 0,1 f1.log -- --port $FP --script "$T/app.lk"
is "FastCGI (a thread per request): 2 CPUs × 4"           "$(workers_in f1.log)" 8

echo "== the database pool opens connections as needed"
start "$ALL" p1.log -- --mode http --port $P; waitup
is "db::connect opens one connection"                     "$(curl -s --max-time 5 "http://127.0.0.1:$P/pool")" 1
cp=(); for i in 1 2 3 4 5 6; do curl -s --max-time 30 -o "$T/s$i" "http://127.0.0.1:$P/slow" & cp+=($!); done; wait "${cp[@]}"   # (yalnız curl'ler; sunucu da arka planda)
ok=1; for i in 1 2 3 4 5 6; do [ "$(cat "$T/s$i")" = 1500000 ] || ok=0; done
is "six parallel queries all answered"                    "$ok" 1
n=$(curl -s --max-time 5 "http://127.0.0.1:$P/pool")
[ "$n" -gt 1 ] && [ "$n" -le 4 ] && echo "  OK   the pool grew under load ($n), within SQLite's 4" || { echo "  FAIL pool size under load: $n"; fail=1; }
start "$ALL" p2.log LOOK_DB_POOL_SIZE=1 -- --mode http --port $P; waitup
cp=(); for i in 1 2 3 4; do curl -s --max-time 30 -o "$T/s$i" "http://127.0.0.1:$P/slow" & cp+=($!); done; wait "${cp[@]}"
ok=1; for i in 1 2 3 4; do [ "$(cat "$T/s$i")" = 1500000 ] || ok=0; done
is "LOOK_DB_POOL_SIZE=1: four parallel queries all answered" "$ok" 1
is "… on one connection"                                  "$(curl -s --max-time 5 "http://127.0.0.1:$P/pool")" 1

echo "== no idle threads (LOOK_DB_ASYNC_THREADS is gone)"
# 1.0.x ayrıca işçi sayısı kadar "database" thread'i açıyordu; hiçbir şey onları kullanmıyordu.
# Thread'lerin adı yok: varsayılanla açılan thread sayısı, eskiden havuzu kapatan
# LOOK_DB_ASYNC_THREADS=0 ile açılanla AYNI olmalı (eskisi işçi sayısı kadar fazlaydı).
threads() { ls /proc/$pid/task | wc -l; }
start 0,1 t0.log LOOK_DB_ASYNC_THREADS=0 -- --mode http --port $P; waitup; sleep 0.5; base=$(threads)
start 0,1 t1.log -- --mode http --port $P; waitup; sleep 0.5
is "default start: no extra idle threads (threads with the old pool switched off: $base)" "$(threads)" "$base"
start 0,1 t2.log LOOK_DB_ASYNC_THREADS=4 -- --mode http --port $P; waitup; sleep 0.5
grep -q "LOOK_DB_ASYNC_THREADS is no longer used" "$T/t2.log" && echo "  OK   LOOK_DB_ASYNC_THREADS set: a warning at start" \
    || { echo "  FAIL no warning for LOOK_DB_ASYNC_THREADS"; fail=1; }
is "… and it starts no threads"                           "$(threads)" "$base"

echo "== a database that cannot be opened still stops the start"
printf '$db = db::connect("sqlite:///nonexistent-dir/x/y.db")\nroute("GET", "/", fn() => response::text("up"))\n' > "$T/bad.lk"
( cd "$T" && timeout 10 env -i PATH=/usr/bin:/bin "$LKF" --mode http --port 7723 bad.lk >"$T/bad.log" 2>&1 ); rc=$?
[ $rc != 0 ] && [ $rc != 124 ] && echo "  OK   LOOK does not start (exit $rc)" || { echo "  FAIL started anyway (rc=$rc)"; fail=1; }

[ $fail = 0 ] && echo "PASS: workers follow usable CPUs; the pool opens connections as needed" || echo "FAIL: worker and pool defaults"
exit $fail

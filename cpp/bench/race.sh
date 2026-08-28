#!/usr/bin/env bash
# LOOK vs PHP vs Node — full race @ 2CPU/4GB. Honest: LOOK measured default AND tuned
# (LOOK_WORKERS matched to quota, rule 9 — else CFS throttling skews it). cpu.stat on
# every LOOK run. Load-gen (wrk) uncapped; servers --cpus=2. Best-of-3, script local FS.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"; NET=benchnet
LEVELS="50 100 1000"
LK="-v $ROOT/cpp:/look/cpp"; DSN="mysql://root:benchpass@mysqlbench/bench"
CAP="--cpus=2 --memory=4g --ulimit nofile=1048576:1048576"

throttle(){ docker exec srv cat /sys/fs/cgroup/cpu.stat 2>/dev/null | awk '/nr_throttled/{print $2}'; }

sweep(){  # label
  local label="$1"
  local ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' srv)
  # Wait for readiness — the DB apps pre-warm their pool BEFORE listening, so poll the
  # endpoint until it answers (up to ~30s) instead of measuring a not-yet-ready server.
  local body=""
  for _ in $(seq 1 60); do
    body=$(docker run --rm --network $NET curlimages/curl -s -m10 "http://$ip:8080/" 2>/dev/null)
    [ -n "$body" ] && break; sleep 0.5
  done
  local w=$(docker logs srv 2>&1 | grep -oE 'workers=[0-9]+' | head -1)
  printf "  %-26s | %.34s | %s\n" "$label" "sanity:${body:-EMPTY}" "$w"
  # Warmup pass (discarded, identical for every runtime — no runtime is measured cold).
  docker run --rm --network $NET williamyeh/wrk -t8 -c100 -d3s "http://$ip:8080/" >/dev/null 2>&1
  local ram="" t0="" t1=""
  for c in $LEVELS; do
    [ "$c" = "100" ] && t0=$(throttle)
    local best=0 bp99=""
    for i in 1 2 3; do
      local out=$(docker run --rm --network $NET williamyeh/wrk -t8 -c"$c" -d6s --latency "http://$ip:8080/" 2>/dev/null)
      local r=$(echo "$out"|awk '/Requests\/sec/{print $2}')
      [ -n "$r" ] && awk "BEGIN{exit !($r>$best)}" && { best=$r; bp99=$(echo "$out"|grep -A4 Distribution|awk '/99%/{print $2}'); }
    done
    [ "$c" = "100" ] && { t1=$(throttle); ram=$(docker stats --no-stream --format '{{.MemUsage}}' srv 2>/dev/null|cut -d/ -f1); }
    printf "      c=%-5s rps=%-10s p99=%s\n" "$c" "$best" "$bp99"
  done
  local thr="-"; [ -n "$t0" ] && [ -n "$t1" ] && thr=$((t1-t0))
  printf "      RAM=%s  throttled(c=100)=%s\n" "${ram:-NA}" "$thr"
}

look(){ docker rm -f srv >/dev/null 2>&1; docker run -d --name srv --network $NET $CAP $1 $LK look-build bash -c "$2"; sleep 3.5; }
plain(){ docker rm -f srv >/dev/null 2>&1; docker run -d --name srv --network $NET $CAP $1; sleep 3.5; }

echo "=================================================================="
echo " RACE @2CPU/4GB — LOOK vs Node vs PHP (best-of-3, cpu.stat, local FS)"
echo "=================================================================="
echo; echo "########## WORKLOAD 1: TRIVIAL JSON (CPU-bound) ##########"
look ""                 "cp /look/cpp/bench/bench.lk /root/s.lk && cd /root && exec /look/cpp/build/lk-fcgi --mode http --port 8080 /root/s.lk"; sweep "LOOK default (w=8)"
look "-e LOOK_WORKERS=2" "cp /look/cpp/bench/bench.lk /root/s.lk && cd /root && exec /look/cpp/build/lk-fcgi --mode http --port 8080 /root/s.lk"; sweep "LOOK tuned (w=2)"
plain "-v $ROOT/cpp/bench:/app -w /app node:20-alpine node app.js";                        sweep "Node (cluster x2)"
plain "-v $ROOT/cpp/bench/phproot:/var/www/html trafex/php-nginx:latest";                  sweep "PHP (fpm+nginx)"

echo; echo "########## WORKLOAD 2: DB ENDPOINT (SELECT id -> JSON, I/O-bound) ##########"
look "-e DB_DSN=$DSN"                                             "cp /look/cpp/bench/dbbench/look/dbapp.lk /root/s.lk && cd /root && exec /look/cpp/build/lk-fcgi --mode http --port 8080 /root/s.lk"; sweep "LOOK default (w=8)"
look "-e DB_DSN=$DSN -e LOOK_WORKERS=4 -e LOOK_DB_POOL_SIZE=4 -e LOOK_DB_ASYNC_THREADS=4" "cp /look/cpp/bench/dbbench/look/dbapp.lk /root/s.lk && cd /root && exec /look/cpp/build/lk-fcgi --mode http --port 8080 /root/s.lk"; sweep "LOOK tuned (w=4 peak)"
plain "-e DB_HOST=mysqlbench -e DB_PASS=benchpass -v $ROOT/cpp/bench/dbbench/node:/app -w /app node:20-alpine node dbapp.js"; sweep "Node (cluster x2 + mysql2)"
plain "-e DB_HOST=mysqlbench -e DB_PASS=benchpass -v $ROOT/cpp/bench/dbbench/php:/var/www/html trafex/php-nginx:latest";      sweep "PHP (fpm+nginx + mysqli)"
docker rm -f srv >/dev/null 2>&1
echo; echo "DONE"

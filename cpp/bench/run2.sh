#!/usr/bin/env bash
# LOOK vs Node vs PHP — 2 CPU / 4 GB. Definitive run.
#   * keep-alive throughput sweep: wrk -t4, best-of-3 (max rps), 6s
#   * non-keepalive @c=100: ab (shows the inversion)
#   * 100k requests: ab -n 100000 -c 200 (non-keepalive, reliable)
# Servers capped --cpus=2 --memory=4g; load-gen uncapped on remaining host CPUs.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"; NET=benchnet; CPUS=2; MEM=4g
UL="--ulimit nofile=1048576:1048576"
LEVELS="50 100 500 1000 10000 20000"

cleanup(){ docker rm -f srv >/dev/null 2>&1; }
trap cleanup EXIT
docker network create $NET >/dev/null 2>&1 || true

ip_of(){ docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' srv; }

best_wrk(){ # ip conc  -> "maxrps p99 err"
  local ip="$1" c="$2" t=4; [ "$c" -lt 4 ] && t="$c"
  local lines=""
  for i in 1 2 3; do
    local out=$(docker run --rm --network $NET $UL williamyeh/wrk -t$t -c"$c" -d6s --latency --timeout 20s "http://$ip:8080/" 2>/dev/null)
    local rps=$(echo "$out" | awk '/Requests\/sec/{print $2}')
    local p99=$(echo "$out" | grep -A4 'Latency Distribution' | awk '/99%/{print $2}')
    local err=$(echo "$out" | awk '/Socket errors/{sub(/Socket errors: /,"");print;exit}')
    lines+="${rps:-0} ${p99:-NA} ${err:-clean}|"
  done
  echo "$lines" | tr '|' '\n' | grep -v '^$' | sort -gr | head -1
}

bench(){ # name
  local name="$1" ip=$(ip_of)
  local body=$(docker run --rm --network $NET curlimages/curl -s -m 10 "http://$ip:8080/" 2>/dev/null)
  echo "### $name   (sanity: ${body:-EMPTY})"
  printf "  %-7s %12s %12s %s\n" "conc" "req/s(max3)" "p99" "errors"
  local ram=""
  for c in $LEVELS; do
    local r=$(best_wrk "$ip" "$c")
    printf "  %-7s %12s %12s %s\n" "$c" "$(echo $r|awk '{print $1}')" "$(echo $r|awk '{print $2}')" "$(echo $r|cut -d' ' -f3-)"
    [ "$c" = "1000" ] && ram=$(docker stats --no-stream --format '{{.MemUsage}}' srv 2>/dev/null)
  done
  echo "  RAM @c=1000: ${ram:-NA}"
  # non-keepalive @c=100 (inversion axis)
  local abn=$(docker run --rm --network $NET $UL jordi/ab -n 20000 -c 100 "http://$ip:8080/" 2>&1 | awk '/Requests per second/{print $4}')
  echo "  non-keepalive @c=100 (ab): ${abn:-NA} req/s"
  # 100k requests
  local ab=$(docker run --rm --network $NET $UL jordi/ab -n 100000 -c 200 "http://$ip:8080/" 2>&1)
  echo "  100k req (ab -c200, no-ka): $(echo "$ab"|awk '/Requests per second/{print $4}') req/s, $(echo "$ab"|awk '/Time taken/{print $5}')s, failed=$(echo "$ab"|awk '/Failed requests/{print $3}'), non2xx=$(echo "$ab"|awk '/Non-2xx/{print $4}'|grep .||echo 0)"
  echo
}

echo "=================================================================="
echo " LOOK vs Node vs PHP  —  server=2CPU/4GB, wrk-t4 best-of-3, 6s/lvl"
echo "=================================================================="
echo
cleanup
docker run -d --name srv --network $NET --cpus=$CPUS --memory=$MEM $UL -v "$ROOT/cpp:/look/cpp" look-build bash -c 'cp /look/cpp/bench/bench.lk /root/b.lk && cd /root && exec /look/cpp/build/lk-fcgi --mode http --port 8080 /root/b.lk' >/dev/null; sleep 3
bench "LOOK 1.0 (lk-fcgi --mode http, script on local FS)"; cleanup

docker run -d --name srv --network $NET --cpus=$CPUS --memory=$MEM $UL -v "$ROOT/cpp/bench:/app" -w /app node:20-alpine node app.js >/dev/null; sleep 3
bench "Node.js 20 (cluster x2)"; cleanup

docker run -d --name srv --network $NET --cpus=$CPUS --memory=$MEM $UL -v "$ROOT/cpp/bench/phproot:/var/www/html" trafex/php-nginx:latest >/dev/null; sleep 4
bench "PHP 8.3 (php-fpm + nginx, prod)"; cleanup

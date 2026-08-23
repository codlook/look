#!/usr/bin/env bash
# LOOK vs Node vs PHP — 2 CPU / 4 GB, direct HTTP servers, trivial JSON endpoint.
# Server containers limited to --cpus=2 --memory=4g; wrk load-gen runs UNCAPPED on
# the remaining host CPUs so the generator is never the bottleneck.
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"                       # host path to repo root (…/look)
NET=benchnet
CPUS=2; MEM=4g
UL="--ulimit nofile=1048576:1048576"
LEVELS="50 100 500 1000 10000 20000"
DUR=10
WRK="--network $NET $UL williamyeh/wrk"

cleanup() { docker rm -f look_srv node_srv php_srv >/dev/null 2>&1; }
trap cleanup EXIT
cleanup
docker network create $NET >/dev/null 2>&1 || true

start_look() { docker run -d --name look_srv --network $NET --cpus=$CPUS --memory=$MEM $UL \
  -v "$ROOT/cpp:/look/cpp" -w /look/cpp/bench look-build \
  /look/cpp/build/lk-fcgi --mode http --port 8080 bench.lk >/dev/null; }
start_node() { docker run -d --name node_srv --network $NET --cpus=$CPUS --memory=$MEM $UL \
  -v "$ROOT/cpp/bench:/app" -w /app node:20-alpine node app.js >/dev/null; }
start_php()  { docker run -d --name php_srv --network $NET --cpus=$CPUS --memory=$MEM $UL \
  -v "$ROOT/cpp/bench/app.php:/var/www/html/index.php:ro" trafex/php-nginx:latest >/dev/null; }

bench_one() {   # $1=name $2=container
  local name="$1" cont="$2"
  local host=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$cont")
  # sanity
  local body=$(docker run --rm --network $NET curlimages/curl -s -m 10 "http://$host:8080/" 2>/dev/null)
  echo "### $name  (sanity: ${body:-EMPTY}  ip=$host)"
  printf "  %-8s %12s %12s %12s %10s\n" "conc" "req/s" "avg_lat" "p99_lat" "errors"
  local ram_sampled=""
  for c in $LEVELS; do
    local t=8; [ "$c" -lt 8 ] && t="$c"
    local out=$(docker run --rm $WRK -t$t -c"$c" -d"${DUR}s" --latency --timeout 20s "http://$host:8080/" 2>/dev/null)
    local rps=$(echo "$out" | awk '/Requests\/sec/{print $2}')
    local avg=$(echo "$out" | awk '/Latency/{print $2; exit}')
    local p99=$(echo "$out" | grep -A4 'Latency Distribution' | awk '/99%/{print $2}')
    local err=$(echo "$out" | awk '/Socket errors/{$1="";print;exit}'); [ -z "$err" ] && err="0"
    printf "  %-8s %12s %12s %12s %10s\n" "$c" "${rps:-NA}" "${avg:-NA}" "${p99:-NA}" "$(echo $err|tr -s ' ')"
    if [ "$c" = "1000" ]; then ram_sampled=$(docker stats --no-stream --format '{{.MemUsage}}' "$cont" 2>/dev/null); fi
  done
  echo "  RAM @c=1000: ${ram_sampled:-NA}"
  # 100k istek — sabit sayı (ab, keep-alive YOK, c=200; entrypoint=ab → args direkt)
  local ab=$(docker run --rm --network $NET $UL jordi/ab -n 100000 -c 200 "http://$host:8080/" 2>&1)
  local abrps=$(echo "$ab" | awk '/Requests per second/{print $4}')
  local abtime=$(echo "$ab" | awk '/Time taken for tests/{print $5}')
  local abok=$(echo "$ab" | awk '/Complete requests/{print $3}')
  local abfail=$(echo "$ab" | awk '/Failed requests/{print $3}')
  echo "  100k istek (ab -c200, keep-alive YOK): ${abrps:-NA} req/s, ${abtime:-NA}s toplam, complete=${abok:-NA}, failed=${abfail:-NA}"
  echo
}

echo "======================================================================"
echo " LOOK vs Node vs PHP — server=2CPU/4GB, wrk uncapped, ${DUR}s/level"
echo "======================================================================"
echo
start_look; sleep 3; bench_one "LOOK (lk-fcgi --mode http)" look_srv; docker rm -f look_srv >/dev/null 2>&1
start_node; sleep 3; bench_one "Node.js 20 (cluster x2)"     node_srv; docker rm -f node_srv >/dev/null 2>&1
start_php;  sleep 5; bench_one "PHP 8.3 (nginx + php-fpm, prod)" php_srv;  docker rm -f php_srv  >/dev/null 2>&1

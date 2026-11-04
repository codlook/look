#!/usr/bin/env bash
# LOOK 2 vs LOOK 1 vs Node vs Go vs Elixir vs PHP — the same two endpoints, the same container shape.
#   1) trivial JSON     GET / → {"ok":true,"msg":"hello"}
#   2) one DB row       GET / → SELECT id, name, email FROM users WHERE id = 42 (MySQL 8) → JSON
# Servers: --cpus=2 --memory=4g. Load generator (wrk): separate, uncapped container. Warm-up, then
# best-of-3 per concurrency level. RSS (docker stats) after c=100 and after c=1000, CPU throttling
# (cpu.stat nr_throttled) over the c=100 runs — BENCHMARK.md hygiene rule 9.
# Every runtime runs its production default; the only knob is the one each needs to see 2 CPUs:
# GOMAXPROCS=2 (Go 1.22 does not read the cgroup quota). LOOK, Node (cluster ×2), BEAM and
# PHP-FPM run as shipped. Elixir's MyXQL pool gets socket_options: [nodelay: true]: without it every
# query waited ~40 ms on Nagle + delayed ACK (360 req/s instead of 15k) — a setting any user would fix.
# Usage: race2.sh <look repo root> <LOOK 2 bin dir> <LOOK 1 bin dir>     (bin dir: lk-fcgi inside)
#   needs images: look-build, look-bench-go, look-bench-elixir (cpp/bench/{go,elixir}/Dockerfile),
#   node:22-alpine, trafex/php-nginx, mysql:8, williamyeh/wrk, curlimages/curl
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"; V2="$2"; V1="$3"; NET=benchnet
LEVELS="${LEVELS:-50 100 1000}"
CAP="--cpus=2 --memory=4g --ulimit nofile=1048576:1048576"
DBENV="-e DB_HOST=mysqlbench -e DB_PASS=benchpass"; DSN="mysql://root:benchpass@mysqlbench/bench"
SECRET="-e LOOK_SECRET=bench-only-0123456789abcdef0123456789abcdef"

docker network inspect $NET >/dev/null 2>&1 || docker network create $NET >/dev/null
setup_db() {
  docker rm -f mysqlbench >/dev/null 2>&1
  docker run -d --name mysqlbench --network $NET -e MYSQL_ROOT_PASSWORD=benchpass -e MYSQL_DATABASE=bench \
    mysql:8 --max-connections=2000 >/dev/null
  for _ in $(seq 1 120); do docker exec mysqlbench mysql -uroot -pbenchpass -e "SELECT 1" bench >/dev/null 2>&1 && break; sleep 1; done
  docker exec mysqlbench mysql -uroot -pbenchpass bench -e "
    CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), email VARCHAR(128));
    INSERT INTO users WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 1000)
      SELECT n, CONCAT('user', n), CONCAT('user', n, '@example.com') FROM s;" 2>/dev/null
}

throttle(){ docker exec srv cat /sys/fs/cgroup/cpu.stat 2>/dev/null | awk '/nr_throttled/{print $2}'; }
ram(){ docker stats --no-stream --format '{{.MemUsage}}' srv 2>/dev/null | cut -d/ -f1 | tr -d ' '; }

sweep(){  # label
  local label="$1"
  local ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' srv)
  local body=""
  for _ in $(seq 1 120); do
    body=$(docker run --rm --network $NET curlimages/curl -s -m10 "http://$ip:8080/" 2>/dev/null)
    [ -n "$body" ] && break; sleep 0.5
  done
  printf "  %-22s | %.48s\n" "$label" "sanity:${body:-EMPTY}"
  [ -z "$body" ] && { docker logs srv 2>&1 | tail -5 | sed 's/^/      /'; return; }
  docker run --rm --network $NET williamyeh/wrk -t8 -c100 -d5s "http://$ip:8080/" >/dev/null 2>&1   # warm-up
  local idle=$(ram) r100="" r1000="" t0="" t1=""
  for c in $LEVELS; do
    [ "$c" = 100 ] && t0=$(throttle)
    local best=0 bp50="" bp99="" berr=""
    for i in 1 2 3; do
      local out=$(docker run --rm --network $NET --ulimit nofile=1048576:1048576 williamyeh/wrk -t8 -c"$c" -d8s --latency "http://$ip:8080/" 2>/dev/null)
      local r=$(echo "$out" | awk '/Requests\/sec/{print $2}')
      if [ -n "$r" ] && awk "BEGIN{exit !($r>$best)}"; then
        best=$r
        bp50=$(echo "$out" | grep -A4 Distribution | awk '/50%/{print $2}')
        bp99=$(echo "$out" | grep -A4 Distribution | awk '/99%/{print $2}')
        berr=$(echo "$out" | awk '/Non-2xx/{printf "non2xx=%s ", $NF} /Socket errors/{sub(/.*Socket errors: */, ""); printf "%s", $0}'); berr=${berr:-0}
      fi
    done
    [ "$c" = 100 ] && { t1=$(throttle); r100=$(ram); }
    [ "$c" = 1000 ] && r1000=$(ram)
    printf "      c=%-5s rps=%-10s p50=%-9s p99=%-9s errors=%s\n" "$c" "$best" "$bp50" "$bp99" "$berr"
  done
  local thr="-"; [ -n "$t0" ] && [ -n "$t1" ] && thr=$((t1 - t0))
  printf "      RSS idle=%s  c=100=%s  c=1000=%s  throttled(c=100)=%s\n" "${idle:-NA}" "${r100:-NA}" "${r1000:-NA}" "$thr"
}

run(){ docker rm -f srv >/dev/null 2>&1; docker run -d --name srv --network $NET $CAP "$@" >/dev/null; sleep 3; }
look(){  # look <bin dir> <app.lk> [docker env...]
  local bin="$1" app="$2"; shift 2
  run $SECRET "$@" -v "$bin:/lkbin:ro" -v "$ROOT/cpp/bench:/bench:ro" look-build \
      bash -c "cp /bench/$app /root/s.lk && cd /root && exec /lkbin/lk-fcgi --mode http --port 8080 /root/s.lk"
}

echo "=================================================================="
echo " RACE @2CPU/4GB — best-of-3 · wrk -t8 -d8s · $(date -u +%F)"
echo " LOOK 2: $(docker run --rm -v "$V2:/b:ro" look-build /b/lk-fcgi --version 2>&1 | head -1)"
echo " LOOK 1: $(docker run --rm -v "$V1:/b:ro" look-build /b/lk-fcgi --version 2>&1 | head -1)"
echo "=================================================================="
echo; echo "########## 1) TRIVIAL JSON ##########"
look "$V2" bench.lk;                                              sweep "LOOK 2"
look "$V1" bench.lk;                                              sweep "LOOK 1 (1.0.45)"
run -v "$ROOT/cpp/bench:/app:ro" -w /app node:22-alpine node app.js;  sweep "Node 22 (cluster x2)"
run -e GOMAXPROCS=2 look-bench-go;                                sweep "Go 1.22 net/http"
run look-bench-elixir;                                            sweep "Elixir (Bandit)"
run -v "$ROOT/cpp/bench/phproot:/var/www/html:ro" trafex/php-nginx:latest; sweep "PHP (fpm+nginx)"

echo; echo "########## 2) ONE DB ROW (MySQL 8) ##########"
setup_db
look "$V2" dbbench/look/dbapp.lk -e DB_DSN=$DSN;                  sweep "LOOK 2"
look "$V1" dbbench/look/dbapp.lk -e DB_DSN=$DSN;                  sweep "LOOK 1 (1.0.45)"
run $DBENV -v "$ROOT/cpp/bench/dbbench/node:/app:ro" -w /app node:22-alpine node dbapp.js; sweep "Node 22 (cluster x2)"
run $DBENV -e GOMAXPROCS=2 look-bench-go;                         sweep "Go 1.22 net/http"
run $DBENV look-bench-elixir;                                     sweep "Elixir (Bandit)"
run $DBENV -v "$ROOT/cpp/bench/dbbench/php:/var/www/html:ro" trafex/php-nginx:latest; sweep "PHP (fpm+nginx)"
docker rm -f srv mysqlbench >/dev/null 2>&1
echo; echo "DONE"

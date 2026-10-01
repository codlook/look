#!/usr/bin/env bash
# Template benchmark — BENCHMARK.md test 10 (T1 static / T2 dynamic / T3 real).
# Same hygiene as cpp/bench/race.sh: server capped at 2 CPU / 4 GB, wrk in a separate
# container, identical warm-up, best-of-3, rps reported together with p99, cpu.stat
# throttling, and a correctness gate before any timing. The server runs with
# LOOK_VM_STRICT=1 so it can never silently measure the tree-walk interpreter.
# Usage: bash cpp/bench/tpl/tpl_race.sh <repo-root> [label]
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"; LABEL="${2:-baseline}"; NET=benchnet
# BIN_DIR: which build to measure (default: the working tree build). Point it at another
# directory under cpp/ to measure an older binary with the SAME app and harness, e.g. for an
# interleaved old/new/old/new comparison in one session.
BIN_DIR="${BIN_DIR:-/look/cpp/build}"
LEVELS="50 200"
CAP="--cpus=2 --memory=4g --ulimit nofile=1048576:1048576"
docker network inspect $NET >/dev/null 2>&1 || docker network create $NET >/dev/null

throttle(){ docker exec srv cat /sys/fs/cgroup/cpu.stat 2>/dev/null | awk '/nr_throttled/{print $2}'; }
curlq(){ docker run --rm --network $NET curlimages/curl -s -m10 "$1" 2>/dev/null; }

docker rm -f srv >/dev/null 2>&1
# The app and its templates are copied to the container's OWN filesystem first. Serving them
# from the bind-mounted repo (Docker Desktop file sharing) makes every template open cost
# milliseconds: the first run of this script did exactly that and measured T1 at ~65 rps and
# T3 at 0 rps (each T3 request opens ~23 template files) — i.e. it measured the host file
# share, not LOOK. It would also have credited any template cache with a fake speed-up.
# This happened twice (here and in an earlier CMS render measurement), so it is enforced by
# mechanism, not memory: the server REFUSES to start if the directory it serves is on any
# mount other than the container's own root filesystem. APP_DIR exists only for the positive
# control (point it at the bind mount and the run must refuse).
docker run -d --name srv --network $NET $CAP -e LOOK_WORKERS=2 -e LOOK_VM_STRICT=1 \
  -e APP_DIR="${APP_DIR:-}" -e BIN_DIR="$BIN_DIR" -v "$ROOT/cpp:/look/cpp" look-build bash -c '
    if [ -z "$APP_DIR" ]; then cp -r /look/cpp/bench/tpl /root/tpl && APP_DIR=/root/tpl; fi
    cd "$APP_DIR" || exit 3
    mp=$(findmnt -n -o TARGET -T .)
    if [ "$mp" != "/" ]; then echo "REFUSE: served app dir $APP_DIR is on mount $mp (host file share skews every result)"; exit 3; fi
    exec "$BIN_DIR/lk-fcgi" --mode http --port 8080 app.lk' >/dev/null
IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' srv)
for _ in $(seq 1 60); do [ -n "$(curlq http://$IP:8080/t1)" ] && break; docker logs srv 2>&1 | grep -q REFUSE && break; sleep 0.5; done
if docker logs srv 2>&1 | grep -q REFUSE; then
  echo "FAIL: $(docker logs srv 2>&1 | grep REFUSE)"; docker rm -f srv >/dev/null 2>&1; exit 1
fi

# Correctness gate — never time a wrong page or an interpreter fallback.
docker logs srv 2>&1 | grep -q "^\[BYTECODE\] OK" || { echo "FAIL: server is not running on the VM"; docker logs srv | tail -5; exit 1; }
t3=$(curlq http://$IP:8080/t3)
echo "$t3" | grep -q "&lt;script&gt;" && ! echo "$t3" | grep -q "<script>alert" \
  && [ "$(echo "$t3" | grep -o 'class="card"' | wc -l)" = "20" ] \
  || { echo "FAIL: /t3 output is wrong (escaping or row count)"; exit 1; }

echo "=================================================================="
echo " TEMPLATE BENCH [$LABEL] — LOOK $(docker exec srv "$BIN_DIR/lk" --version 2>/dev/null)"
echo " 2 CPU / 4 GB, LOOK_WORKERS=2, best-of-3, 6 s per run"
echo "=================================================================="
for ep in t1 t2 t3; do
  docker run --rm --network $NET williamyeh/wrk -t4 -c100 -d3s "http://$IP:8080/$ep" >/dev/null 2>&1   # warm-up
  echo "  /$ep"
  for c in $LEVELS; do
    t0=$(throttle); best=0; bp99=""
    for i in 1 2 3; do
      out=$(docker run --rm --network $NET williamyeh/wrk -t4 -c"$c" -d6s --latency "http://$IP:8080/$ep" 2>/dev/null)
      r=$(echo "$out" | awk '/Requests\/sec/{print $2}')
      [ -n "$r" ] && awk "BEGIN{exit !($r>$best)}" && { best=$r; bp99=$(echo "$out" | grep -A4 Distribution | awk '/99%/{print $2}'); }
    done
    t1=$(throttle)
    printf "      c=%-4s rps=%-10s p99=%-9s throttled=%s\n" "$c" "$best" "$bp99" "$((t1-t0))"
  done
done
printf "  RAM=%s\n" "$(docker stats --no-stream --format '{{.MemUsage}}' srv | cut -d/ -f1)"
docker rm -f srv >/dev/null 2>&1

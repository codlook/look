#!/usr/bin/env bash
# WebSocket race: LOOK vs Node 22 (ws, cluster ×2) vs Go 1.22 (gorilla/websocket), the same echo.
# Each server: --cpus=2 --memory=2g; the load client (Go) runs in its own uncapped container.
#   echo  : N connections, each sends a 64-byte text frame and waits for its echo, 15 s
#   hold  : N connections opened and kept open; RSS read while they are open; then a sample of
#           200 must still echo
# Usage: ws-race.sh <look repo root> <LOOK bin dir>
# LOOK caps open WebSocket/SSE connections per address at 1000 when nothing is in front of it (no
# LOOK_TRUSTED_PROXY); every connection here comes from one client address, so the run sets
# LOOK_HTTP_MAX_CONNS_IP=1000000. Node and Go have no such default.
#   images: look-build, look-ws-go, look-ws-node (Dockerfile.go / Dockerfile.node here), benchnet network
export MSYS_NO_PATHCONV=1
set -u
ROOT="$1"; LKBIN="$2"; NET=benchnet
CAP="--cpus=2 --memory=2g --memory-swap=2g --ulimit nofile=1048576:1048576"
cli() { docker run --rm --network $NET --ulimit nofile=1048576:1048576 --sysctl "net.ipv4.ip_local_port_range=1024 65535" look-ws-go /client "$@"; }
docker network inspect $NET >/dev/null 2>&1 || docker network create $NET >/dev/null

ram() { docker stats --no-stream --format '{{.MemUsage}}' srv 2>/dev/null | cut -d/ -f1 | tr -d ' '; }
run_srv() { docker rm -f srv >/dev/null 2>&1; docker run -d --name srv --network $NET $CAP "$@" >/dev/null; }
race() {   # label
  local ip; ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' srv)
  for _ in $(seq 1 60); do docker run --rm --network $NET curlimages/curl -s -m2 -o /dev/null "http://$ip:8080/" && break; sleep 0.5; done
  local url="ws://$ip:8080/ws"
  echo "  $1   (idle RSS $(ram))"
  cli -url "$url" -mode echo -n 100 -d 3s >/dev/null 2>&1    # warm-up
  for n in 100 1000; do
    echo "      echo  n=$n   $(cli -url "$url" -mode echo -n $n -d 15s 2>&1 | tail -1)   RSS $(ram)"
  done
  for n in 10000 50000; do
    local f; f=$(mktemp)
    ( cli -url "$url" -mode hold -n $n -c 500 -hold 15s > "$f" 2>&1 ) & local cp=$!
    for _ in $(seq 1 240); do grep -q "^HOLD" "$f" && break; sleep 0.5; done
    sleep 3; local r; r=$(ram)
    wait $cp
    echo "      hold  n=$n   $(grep -E '^HOLD' "$f")   $(grep -E '^ALIVE' "$f")   RSS(open)=$r"
    rm -f "$f"; sleep 3
  done
  docker logs srv 2>&1 | grep -ciE "error|exception|abort|killed" | sed 's/^/      server log error lines: /'
}

echo "=================================================================="
echo " WebSocket RACE @2CPU/2GB — $(date -u +%F)   LOOK: $(docker run --rm -v "$LKBIN:/b:ro" look-build /b/lk-fcgi --version 2>&1 | head -1)"
echo "=================================================================="
run_srv -e LOOK_HTTP_MAX_CONNS_IP=1000000 -v "$LKBIN:/b:ro" -v "$ROOT/cpp/bench/ws:/ws:ro" look-build bash -c "cp /ws/echo.lk /root/e.lk && cd /root && exec /b/lk-fcgi --mode http --port 8080 e.lk"
race "LOOK"
run_srv look-ws-node;               race "Node 22 (ws, cluster x2)"
run_srv -e GOMAXPROCS=2 look-ws-go; race "Go 1.22 (gorilla/websocket)"
docker rm -f srv >/dev/null 2>&1
echo "DONE"

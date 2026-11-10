#!/usr/bin/env bash
# The port opens only when the server can serve.
#
# lk-fcgi used to listen while it was still starting: --mode http listened in the HttpServer
# constructor and only then started its workers (fiber mode even closed that socket again and
# opened per-worker ones); the FastCGI mode listened before its thread pool existed. A client that
# connected in between was accepted and got no answer — and under a memory limit, where the
# workers cannot all start, the process then exited with its reason while the connection hung.
# Measured: with no limit at all, a tight probe got an unanswered connection in 49 of 50 starts.
# Here a probe connects as fast as it can while the server starts:
#   * a normal start: the first connection accepted must be answered (fiber and per-connection modes)
#   * a start that must fail (memory limit too small for the workers): no connection is ever
#     accepted, and the process exits with its reason (http and FastCGI)
# Usage: listen_when_ready_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7791}"
printf 'route("GET", "/ok", fn() => response::text("alive"))\n' > "$TMP/a.lk"
fail=0; n=0

# start <limit-MB or 0> <args…>: runs lk-fcgi in the background; pid in $pid
start() {
  local mb=$1; shift
  if [ "$mb" = 0 ]; then ( cd "$TMP" && exec "$FCGI" "$@" a.lk > "$TMP/log" 2>&1 ) & pid=$!
  else ( cd "$TMP" && ulimit -v $((mb * 1024)) && exec "$FCGI" "$@" a.lk > "$TMP/log" 2>&1 ) & pid=$!; fi
}
# probe: connect as fast as possible until accepted or the process is gone (≤ 15 s).
# Prints "accepted" or "gone"; with http=1 sends a request on the accepted connection and prints the reply.
probe() {
  local http=$1 end=$((SECONDS + 15))
  while [ $SECONDS -lt $end ]; do
    kill -0 "$pid" 2>/dev/null || { echo gone; return; }
    if exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then
      if [ "$http" = 1 ]; then
        printf 'GET /ok HTTP/1.0\r\n\r\n' >&3 2>/dev/null
        echo "accepted:$(timeout 5 cat <&3 2>/dev/null | tail -1)"
      else echo accepted; fi
      exec 3<&- 3>&-; return
    fi
  done
  echo timeout
}
stop() { kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""; }

# 1. normal starts: the first accepted connection is answered
for mode in fiber pool; do
  for r in $(seq 1 10); do
    n=$((n + 1))
    if [ $mode = pool ]; then LOOK_FIBER_DISPATCH=0 start 0 --mode http --port "$PORT" --workers 8
    else start 0 --mode http --port "$PORT" --workers 8; fi
    got=$(probe 1 2>/dev/null)
    stop
    [ "$got" = "accepted:alive" ] || { echo "  FAIL $mode start $r: the first accepted connection got '${got#accepted:}' (want 'alive')"; fail=1; }
  done
done

# 2. starts that must fail: never accept, exit with the reason
for spec in "http 1024 --mode http --workers 64" "http-pool 256 --mode http --workers 64" "fcgi 64 --workers 64"; do
  set -- $spec; name=$1; mb=$2; shift 2
  for r in $(seq 1 10); do
    n=$((n + 1))
    if [ $name = http-pool ]; then LOOK_FIBER_DISPATCH=0 start "$mb" "$@" --port "$PORT"; else start "$mb" "$@" --port "$PORT"; fi
    got=$(probe 0 2>/dev/null)
    if [ "$got" = gone ]; then
      wait "$pid"; rc=$?; pid=""
      { [ $rc != 0 ] && grep -q "could not start\|cannot\|Cannot allocate" "$TMP/log"; } \
        || { echo "  FAIL $name ${mb} MB start $r: exited $rc without its reason: $(tail -2 "$TMP/log" | tr '\n' ' ')"; fail=1; }
    else
      echo "  FAIL $name ${mb} MB start $r: a start that cannot serve accepted a connection ($got)"; fail=1
      stop
    fi
  done
done

[ $fail = 0 ] && echo "PASS: the port opens only when the server can serve ($n starts)" \
              || { echo "FAIL: a connection was accepted before the server could serve"; exit 1; }

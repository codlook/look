#!/usr/bin/env bash
# Bounded-concurrency HANG regression smoke test (worker-pool + fiber dispatch).
#
# ── What it protects ──────────────────────────────────────────────────────────
# lk-fcgi --mode http comes with two dispatch models:
#   • worker-pool (DEFAULT)          — N worker threads, each blocks on accept+handle.
#   • fiber burst (LOOK_FIBER_DISPATCH=1, Linux) — SO_REUSEPORT + fiber acceptor.
# In the past the fiber mode HUNG at c=100: the acceptor did not accept new connections while
# waiting for keep-alive fibers. D Fix #2 (the acceptor is now a fiber +
# 15s idle-timeout) fixed it — but there was no LOAD-REGRESSION guard. This test is that guard.
#
# ── POSITIVE CONTROL ──────────────────────────────────────────────────────────
# The test REALLY exercises concurrency: CONC connections are opened AT THE SAME TIME, each
# sends PERCON requests over keep-alive (the accept + keep-alive-recv axis —
# exactly the root cause of the hang). If a hang EXISTED: the starved connections cannot finish their work within CONN_TIMEOUT
# → a missing 200 count AND/OR probe_alive() after the load is
# false → FAIL. It catches the class that a single-request smoke test cannot.
# (To prove this: leave the pool starved with workers=1 + a low timeout and verify
#  that you see a FAIL — not in the README, a manual positive-control step.)
#
# ── FLAKY WARNING ─────────────────────────────────────────────────────────────
# A load test is by nature sensitive to machine load. This harness reduces the flaky margin with LOW concurrency
# (default c=50) + a GENEROUS timeout (8s/conn) but still catches a hang
# regression (a hang is not seconds, it is an INDEFINITE stall). Still,
# c=100 can be noisy on a 2-core shared CI runner; c=50 is recommended there.
#
# Usage: bash concurrency_smoke.sh [lk-fcgi_path] [concurrency] [reqs_per_conn]
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
FCGI="${1:-$DIR/../build/lk-fcgi}"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"
CONC="${2:-50}"
PERCON="${3:-20}"
PORT=7402
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A tiny, CPU-light app: /ping quickly returns 200 (isolates the hang — we measure
# dispatch, not app slowness).
cat > "$TMP/app.lk" <<'LK'
route("GET", "/ping", function() {
  return response::text("ok")
})
LK

cd "$TMP"

run_mode() {
  local label="$1"; shift
  local env_pfx="$1"; shift
  # env_pfx: "" (pool) or "LOOK_FIBER_DISPATCH=1"
  echo "[$label] starting server..."
  if [ -n "$env_pfx" ]; then export $env_pfx; fi
  "$FCGI" --mode http --port $PORT app.lk >"$TMP/srv.log" 2>&1 &
  local srv=$!
  # Wait until it is ready (/ping 200 via curl)
  local up=0
  for i in $(seq 1 40); do
    if curl -s --max-time 1 "http://127.0.0.1:$PORT/ping" 2>/dev/null | grep -q ok; then up=1; break; fi
    sleep 0.25
  done
  if [ "$up" != "1" ]; then
    echo "  FAIL [$label]: server did not come up"; sed 's/^/    srv: /' "$TMP/srv.log"
    kill $srv 2>/dev/null; wait $srv 2>/dev/null
    if [ -n "$env_pfx" ]; then unset ${env_pfx%%=*}; fi
    return 1
  fi
  python3 "$DIR/concurrency_smoke.py" $PORT $CONC $PERCON
  local rc=$?
  kill $srv 2>/dev/null; wait $srv 2>/dev/null; sleep 0.5
  if [ -n "$env_pfx" ]; then unset ${env_pfx%%=*}; fi
  return $rc
}

fail=0
run_mode "worker-pool" ""                       || fail=1
run_mode "fiber"       "LOOK_FIBER_DISPATCH=1"  || fail=1

if [ "$fail" = "0" ]; then
  echo "ALL PASS: neither dispatch mode hung at c=$CONC."
else
  echo "NOT PASSED: at least one mode hung or returned an incomplete response."
fi
exit $fail

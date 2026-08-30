#!/usr/bin/env bash
# Regression: an lk-fcgi (--mode http) app that has BOTH a file include (use "file.lk")
# AND a top-level timer::every() must bind the port and serve. Previously it HUNG at
# startup — timer::every thread-clones its callback's closure environment, and every
# function the include defined shared one isolated closure env, which clone_for_thread
# re-cloned once per function → O(N!) blowup that never returned "Listening".
# POSITIVE CONTROL: the include carries >10 sibling functions, so the old code path
# would hang here (never bind) and this test would FAIL on timeout.
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7794}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 0.3; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; }
trap cleanup EXIT
command -v curl >/dev/null 2>&1 || { echo "  (atlandi: curl gerekli)"; exit 0; }

"$FCGI" --mode http --port "$PORT" "$HERE/timer_include_app.lk" >/dev/null 2>&1 &
SRV=$!

# Poll for up to ~6s. A hung startup never binds → loop exhausts → FAIL.
code=""
for i in $(seq 1 60); do
  # Server process died (e.g. setup error) → fail fast.
  kill -0 "$SRV" 2>/dev/null || { echo "FAIL: lk-fcgi exited before binding"; exit 1; }
  code=$(curl -s -o /dev/null -w "%{http_code}" -m 2 "http://127.0.0.1:$PORT/x" 2>/dev/null)
  [ "$code" = "200" ] && break
  sleep 0.1
done

if [ "$code" = "200" ]; then
  echo "PASS: file-include + timer::every binds and serves (http=200)"
  exit 0
fi
echo "FAIL: app did not bind (last http=[$code]) — likely the clone_for_thread hang regressed"
exit 1

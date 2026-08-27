#!/bin/bash
# Fiber-mode per-request web-context correctness guard.
#
# Under LOOK_FIBER_DISPATCH=1 a single worker thread interleaves many connection
# fibers cooperatively. Each request's handler reads its own input via
# request::get("v") AFTER yielding at an http::get() call. If the per-request
# module builtins / web-context are bound to a copy that is SHARED across
# interleaved fibers, one fiber's resumed read observes another fiber's context
# and returns the wrong value. This test fires many concurrent requests with
# unique values and asserts every response equals its own input.
#
# Usage: run.sh <lk-fcgi> [worker-pool|fiber]
set -u
BIN="${1:?path to look-fcgi/lk-fcgi binary}"
MODE="${2:-fiber}"
PORT=7611
DIR="$(cd "$(dirname "$0")" && pwd)"

[ "$MODE" = fiber ] && export LOOK_FIBER_DISPATCH=1
# /echo's http::get() targets loopback (the slow upstream) — allow it past SSRF.
export LOOK_ALLOW_SSRF=1
# Optional: LOOK_BYTECODE=0 forces the tree-walk interpreter fiber path.
WORKERS="${LOOK_WORKERS:-2}"

# Slow upstream that /echo's http::get() hits — widens the fiber yield window so
# many requests are suspended simultaneously (guarantees the shared web-context
# gets overwritten before any given fiber resumes).
python3 "$DIR/slow_server.py" 7699 0.4 >/tmp/fctx_slow.log 2>&1 &
SLOW=$!
for _ in $(seq 1 40); do
  (exec 3<>/dev/tcp/127.0.0.1/7699) 2>/dev/null && { exec 3>&- 3<&-; break; }
  sleep 0.1
done

# Force interleave onto few threads so fibers actually share a dispatch copy.
"$BIN" --mode http --port "$PORT" --workers "$WORKERS" "$DIR/app.lk" >/tmp/fctx_o.log 2>/tmp/fctx_e.log &
SRV=$!
# wait for readiness
for _ in $(seq 1 40); do
  curl -s "http://127.0.0.1:$PORT/slow" >/dev/null 2>&1 && break
  sleep 0.25
done

N=80
RESP=/tmp/fctx_resp
rm -rf "$RESP"; mkdir -p "$RESP"
pids=""
for i in $(seq 1 $N); do
  ( r=$(curl -s "http://127.0.0.1:$PORT/echo?v=req$i"); printf '%s' "$r" > "$RESP/$i" ) &
  pids="$pids $!"
done
wait $pids
kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null
kill "$SLOW" 2>/dev/null; wait "$SLOW" 2>/dev/null

bad=0; checked=0; empty=0
for i in $(seq 1 $N); do
  got=$(cat "$RESP/$i" 2>/dev/null)
  checked=$((checked+1))
  if [ "$got" != "req$i" ]; then
    bad=$((bad+1))
    [ -z "$got" ] && empty=$((empty+1))
    [ $bad -le 5 ] && echo "MISMATCH: expected req$i got '$got'"
  fi
done
echo "MODE=$MODE workers=$WORKERS bytecode=${LOOK_BYTECODE:-1} checked=$checked mismatches=$bad (empty=$empty)"
[ "$bad" -eq 0 ] || exit 1

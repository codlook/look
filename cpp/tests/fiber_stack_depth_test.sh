#!/usr/bin/env bash
# Deep recursion inside a request ends in a clean error, with fibers as with threads.
#
# 1.0.20 made fibers the default dispatch on Linux. Fiber stacks were 128 KB; on the tree-walk
# engine about 100 levels of recursion overflowed one and the whole process died with a
# segmentation fault before the call-depth limit could answer with an error. Thread stacks are
# 8 MB, where the limit answers first. Fiber stacks are 8 MB now too (address space only:
# memory is used for the pages a stack touches). Measured before the release: 100 levels
# killed the server on the tree-walk engine with fibers; with threads they were answered.
# Not meaningful under ASan, which makes every frame larger.
# Usage: fiber_stack_depth_test.sh <lk-fcgi>
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
PORT="${PORT:-7817}"
cat > "$TMP/app.lk" <<'LK'
function down($n) { if ($n <= 0) { return 0 } return 1 + down($n - 1) }
route("GET", "/deep/{n}", function($n) { return response::text("depth=" . down($n + 0)) })
route("GET", "/ok", fn() => response::text("alive"))
LK
fail=0
for mode in "" "LOOK_FIBER_DISPATCH=0"; do   # default (fibers), then the worker pool
    ( cd "$TMP" && exec env $mode "$FCGI" --mode http --port "$PORT" --workers 2 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$PORT/ok" && break; sleep 0.1; done
    got=""; for d in 100 200 5000; do got="$got $(curl -s -m 10 -o /dev/null -w '%{http_code}' "localhost:$PORT/deep/$d")"; done
    up=no; kill -0 "$pid" 2>/dev/null && up=yes
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    if [ "$got" = " 200 200 500" ] && [ $up = yes ]; then echo "  OK   [${mode:-default}] depth 100 and 200 answered, 5000 is a clean 500, the server is up"
    else echo "  FAIL [${mode:-default}] depth 100/200/5000 -> [$got ] server up: $up"; fail=1; fi
done
[ $fail = 0 ] && echo "PASS: deep recursion ends in a clean error on every dispatch" || { echo "FAIL: deep recursion took the server down"; exit 1; }

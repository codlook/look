#!/bin/bash
# ── Fiber-mode per-request web-context correctness regression guard ──────────
#
# Regression target: under LOOK_FIBER_DISPATCH=1 the tree-walk interpreter
# dispatch path bound request::/response:: to a THREAD_LOCAL, shared dispatch
# copy. Because one worker thread interleaves many connection fibers, a handler
# that yielded (db::query / http_client) at an I/O point let another fiber
# rebind that shared copy to ITS request — so the first fiber's resumed
# request::get() returned another user's data. The fix gives every fiber its own
# dispatch copy in fiber mode (the VM path was already safe via a per-request
# builtins snapshot). See look_app_dispatch() in cpp/src/http_main.cpp.
#
# This guard runs the two fiber configurations and asserts zero cross-request
# leakage. The interpreter+fiber row is the one that used to fail 80/80.
#
# Requirements: Linux (fiber-burst is Linux-only) + python3 + curl. Skips (exit
# 0 with a notice) when unavailable so it is safe to call from any CI job.
set -u
BIN="${1:?usage: guard.sh <lk-fcgi binary>}"
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(uname -s)" != "Linux" ]; then
  echo "SKIP fiber_ctx guard: fiber-burst mode is Linux-only"; exit 0
fi
if ! command -v python3 >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  echo "SKIP fiber_ctx guard: python3/curl required"; exit 0
fi

rc=0
echo "== interpreter path + fiber (regression case) =="
LOOK_BYTECODE=0 bash "$DIR/run.sh" "$BIN" fiber || rc=1
echo "== VM path + fiber =="
bash "$DIR/run.sh" "$BIN" fiber || rc=1

if [ "$rc" -ne 0 ]; then
  echo "FAIL: fiber-mode web-context leak detected"; exit 1
fi
echo "OK: no cross-request web-context leakage under fiber mode"

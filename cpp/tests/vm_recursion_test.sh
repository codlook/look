#!/usr/bin/env bash
# vm_recursion_test.sh — is VM recursion depth LIMITED (bug 57) and is the limit in the right place.
#
# The MAX_CALL_DEPTH check was only in call_closure (host→VM); the bytecode CALL opcode pushed the frame
# without a limit → direct LOOK recursion was UNLIMITED: runaway recursion ran until memory was exhausted
# instead of a graceful "Stack overflow" (OOM DoS). Fix: the CALL path enforces the limit too.
# 2026-11-09: the limit was raised from 500 to 10,000 calls / an 8 MB call stack (tests/call_depth_test.sh) —
# this guard verifies that infinite recursion is still LIMITED and that deep but finite recursion
# works.
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
printf 'function r($n){ if($n<=0){return 0} return 1+r($n-1) }\nprint(r(9000))\n' > "$TMP/deep.lk"
printf 'function f($n){ return f($n+1) + 1 }\nprint(f(0))\n' > "$TMP/endless.lk"

# Deep but finite: 9000 levels work (limit 10,000, Node ~9k)
out=$(timeout 30 "$LK" "$TMP/deep.lk" 2>&1 | tr '\n' ' ')
if echo "$out" | grep -q "^9000"; then echo "  PASS deep recursion: r(9000) = 9000"
else echo "  FAIL deep recursion: beklenmedik: [$out]"; fail=1; fi

# Infinite: graceful "Stack overflow" (does not run until memory is exhausted), no crash
out=$(timeout 30 "$LK" "$TMP/endless.lk" 2>&1 | tr '\n' ' '); rc=${PIPESTATUS[0]}
if echo "$out" | grep -qi "stack overflow"; then echo "  PASS endless recursion bounded (graceful stack overflow)"
else echo "  FAIL endless recursion: beklenmedik: [$out]"; fail=1; fi
timeout 30 "$LK" "$TMP/endless.lk" >/dev/null 2>&1; rc=$?
if [ "$rc" -ge 124 ]; then echo "  FAIL: VM COKTU ya da asildi (exit=$rc; 139=segfault,137=OOM,124=timeout)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: vm recursion (57 — CALL yolu sinirli, derin sonlu ozyineleme calisir)" || { echo "FAIL: vm recursion"; exit 1; }

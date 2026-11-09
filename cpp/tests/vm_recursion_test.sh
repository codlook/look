#!/usr/bin/env bash
# vm_recursion_test.sh — VM özyineleme derinliği SINIRLI mı (57. bug) ve sınır doğru yerde mi.
#
# MAX_CALL_DEPTH kontrolü yalnız call_closure'daydı (host→VM); bytecode CALL opcode'u frame'i
# limitsiz push ediyordu → doğrudan LOOK özyinelemesi SINIRSIZDI: runaway özyineleme graceful
# "Stack overflow" yerine bellek tükenene dek gidiyordu (OOM DoS). Fix: CALL yolu da sınırı zorlar.
# 2026-11-09: sınır 500'den 1M çağrıya / 256 MB çağrı yığınına çıktı (tests/call_depth_test.sh) —
# bu guard sonsuz özyinelemenin hâlâ SINIRLI olduğunu ve derin ama sonlu özyinelemenin çalıştığını
# doğrular.
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
printf 'function r($n){ if($n<=0){return 0} return 1+r($n-1) }\nprint(r(100000))\n' > "$TMP/deep.lk"
printf 'function f($n){ return f($n+1) + 1 }\nprint(f(0))\n' > "$TMP/endless.lk"

# Derin ama sonlu: 100000 seviye çalışır (Node ~9k'da durur)
out=$(timeout 30 "$LK" "$TMP/deep.lk" 2>&1 | tr '\n' ' ')
if echo "$out" | grep -q "^100000"; then echo "  PASS deep recursion: r(100000) = 100000"
else echo "  FAIL deep recursion: beklenmedik: [$out]"; fail=1; fi

# Sonsuz: graceful "Stack overflow" (bellek tükenene dek gitmez), çökme yok
out=$(timeout 30 "$LK" "$TMP/endless.lk" 2>&1 | tr '\n' ' '); rc=${PIPESTATUS[0]}
if echo "$out" | grep -qi "stack overflow"; then echo "  PASS endless recursion bounded (graceful stack overflow)"
else echo "  FAIL endless recursion: beklenmedik: [$out]"; fail=1; fi
timeout 30 "$LK" "$TMP/endless.lk" >/dev/null 2>&1; rc=$?
if [ "$rc" -ge 124 ]; then echo "  FAIL: VM COKTU ya da asildi (exit=$rc; 139=segfault,137=OOM,124=timeout)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: vm recursion (57 — CALL yolu sinirli, derin sonlu ozyineleme calisir)" || { echo "FAIL: vm recursion"; exit 1; }

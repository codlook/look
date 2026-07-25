#!/usr/bin/env bash
# vm_recursion_test.sh — VM özyineleme derinliği SINIRLI mı (57. bug).
#
# MAX_CALL_DEPTH kontrolü yalnız call_closure'daydı (host→VM); bytecode CALL
# opcode'u frame'i limitsiz push ediyordu → doğrudan LOOK özyinelemesi SINIRSIZDI:
# runaway/derin özyineleme graceful "Stack overflow" yerine bellek tükenene dek
# gidiyordu (OOM DoS) ve tree-walk'un sınırından ayrışıyordu. Fix: CALL yolu da
# MAX_CALL_DEPTH'i zorlar. Bu guard hem VM'in bounded olduğunu hem her iki motorun
# derin özyinelemede graceful hata verdiğini doğrular.
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
printf 'function r($n){ if($n<=0){return 0} return 1+r($n-1) }\nprint(r(100000))\n' > "$TMP/deep.lk"

# VM (default) — derin özyineleme graceful hata vermeli (100000 DÖNMEMELI = unbounded)
vm=$(timeout 15 "$LK" "$TMP/deep.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ')
if echo "$vm" | grep -qi "stack overflow"; then
  echo "  PASS VM: derin ozyineleme bounded (graceful stack overflow)"
elif echo "$vm" | grep -q "100000"; then
  echo "  FAIL VM: r(100000) dondu — CALL yolu MAX_CALL_DEPTH'i zorlamiyor (unbounded/OOM riski)"; fail=1
else
  echo "  FAIL VM: beklenmedik: [$vm]"; fail=1
fi

# tree-walk (LOOK_CLI_VM=0) — o da bounded olmali
tw=$(LOOK_CLI_VM=0 timeout 15 "$LK" "$TMP/deep.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ')
if echo "$tw" | grep -qi "stack overflow"; then
  echo "  PASS tree-walk: derin ozyineleme bounded"
else
  echo "  FAIL tree-walk: beklenmedik: [$tw]"; fail=1
fi

# Sig-crash yok (exit 139/137 olmamali)
timeout 15 "$LK" "$TMP/deep.lk" >/dev/null 2>&1; rc=$?
if [ "$rc" -ge 128 ]; then echo "  FAIL: VM COKTU (exit=$rc; 139=segfault,137=OOM)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: vm recursion (57 — CALL yolu MAX_CALL_DEPTH)" || echo "FAIL: vm recursion"
exit $fail

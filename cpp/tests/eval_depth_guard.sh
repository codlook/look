#!/bin/bash
# B-01 eval/parse depth guard — deep nesting / long binary chain DoS test.
#
# Root cause: '1+1+1+...+1' (N terms) is built ITERATIVELY in the parser (addition() while
# loop) — the expr_depth_ paren/array guard does NOT increase, so it does NOT CATCH it. But the
# resulting AST becomes left-nested to depth N; both interpreter evaluate() and the VM
# compiler walk this skeleton recursively and overflow the C++ stack at ~7000 terms → SIGABRT
# (a CRASH, NOT a clean error). On a web server, attacker-controlled deep input = DoS.
#
# Fix: a MAX_BINOP_CHAIN (1000) counter in the parser's binary-operator loops → when the limit is
# exceeded a clean LookParseError is raised WITHOUT building the AST at all. Neither interpreter nor VM
# sees the deep tree; the process lives (returns 500 on the web).
#
# This test: (a) deep input gives a CLEAN error (rc!=0 but NOT signal/SIGABRT),
#          (b) an expression of normal depth is unaffected (correct result).
# Usage: bash eval_depth_guard.sh [lk_path]
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
LK="${1:-$DIR/../build/look}"
[ -x "$LK" ] || LK="${1:-$DIR/../build-win/Release/lk.exe}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

gen_chain() { awk -v n="$1" 'BEGIN{printf "print(1"; for(i=0;i<n;i++) printf "+1"; print ")"}'; }
gen_paren() { awk -v n="$1" 'BEGIN{printf "print("; for(i=0;i<n;i++) printf "("; printf "1"; for(i=0;i<n;i++) printf ")"; print ")"}'; }

# Run a source file; return the rc. rc>=128 (POSIX) or
# 127 (Windows/bash native crash) → means signal/crash → GUARD FAILS.
run() { # $1=dosya
  "$LK" "$1" >/dev/null 2>&1
  echo $?
}
is_crash() { local rc="$1"; [ "$rc" -ge 128 ] || [ "$rc" = "127" ] || [ "$rc" = "134" ]; }

echo "== Guard: derin girdi TEMİZ hata vermeli (çökme değil) =="
for kind in chain paren; do
  [ "$kind" = chain ] && gen_chain 20000 > "$TMP/deep.lk" || gen_paren 20000 > "$TMP/deep.lk"
  rc=$(run "$TMP/deep.lk")
  if is_crash "$rc"; then echo "  FAIL [$kind] ÇÖKTÜ (rc=$rc — sinyal/SIGABRT)"; fail=1
  elif [ "$rc" = "0" ]; then echo "  FAIL [$kind] guard tetiklenmedi (rc=0)"; fail=1
  else echo "  OK   [$kind] temiz hata (rc=$rc)"; fi
done

echo "== Regresyon: normal derinlikteki ifade doğru çalışmalı =="
gen_chain 500 > "$TMP/ok.lk"   # result 501; far below the guard (1000)
out=$("$LK" "$TMP/ok.lk" 2>/dev/null)
if [ "$out" = "501" ]; then echo "  OK   500-terim zincir = 501"; else echo "  FAIL beklenen 501, gelen '$out'"; fail=1; fi
printf 'print(1+2+3*4-5)\n' > "$TMP/sm.lk"
[ "$("$LK" "$TMP/sm.lk" 2>/dev/null)" = "10" ] && echo "  OK   [VM] karışık küçük ifade = 10" || { echo "  FAIL karışık ifade"; fail=1; }

[ "$fail" = 0 ] && echo "== eval derinlik guard: TÜM TESTLER GEÇTİ ==" || echo "== eval derinlik guard: BAŞARISIZ =="
exit $fail

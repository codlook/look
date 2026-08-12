#!/bin/bash
# B-01 eval/parse derinlik guard'ı — derin iç içe / uzun ikili zincir DoS testi.
#
# Kök neden: '1+1+1+...+1' (N terim) parse'ta İTERATİF kurulur (addition() while
# döngüsü) — expr_depth_ paren/array guard'ı ARTMAZ, bu yüzden onu YAKALAMAZ. Ama
# üretilen AST N derinliğinde sola-nested olur; hem interpreter evaluate() hem VM
# compiler bu iskeleti özyineli gezerek ~7000 terimde C++ stack'ini taşırır → SIGABRT
# (temiz hata DEĞİL, ÇÖKME). Web sunucusunda saldırgan-kontrollü derin girdi = DoS.
#
# Fix: parser'ın ikili-operatör döngülerine MAX_BINOP_CHAIN (1000) sayacı → sınır
# aşılınca AST HİÇ kurulmadan temiz LookParseError. Ne interpreter ne VM derin ağacı
# görür; süreç yaşar (web'de 500 döner).
#
# Bu test: (a) derin girdi TEMİZ hata verir (rc!=0 ama signal/SIGABRT DEĞİL),
#          (b) normal derinlikteki ifade etkilenmez (doğru sonuç).
# Kullanım: bash eval_depth_guard.sh [lk_yolu]
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
LK="${1:-$DIR/../build/look}"
[ -x "$LK" ] || LK="${1:-$DIR/../build-win/Release/lk.exe}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

gen_chain() { awk -v n="$1" 'BEGIN{printf "print(1"; for(i=0;i<n;i++) printf "+1"; print ")"}'; }
gen_paren() { awk -v n="$1" 'BEGIN{printf "print("; for(i=0;i<n;i++) printf "("; printf "1"; for(i=0;i<n;i++) printf ")"; print ")"}'; }

# Bir kaynak dosyayı verilen motorla çalıştır; rc'yi döndür. rc>=128 (POSIX) veya
# 127 (Windows/bash native crash) → sinyal/çökme demek → GUARD BAŞARISIZ.
run() { # $1=dosya $2=VM(1)/interp(0)
  if [ "$2" = "0" ]; then LOOK_CLI_VM=0 "$LK" "$1" >/dev/null 2>&1; else "$LK" "$1" >/dev/null 2>&1; fi
  echo $?
}
is_crash() { local rc="$1"; [ "$rc" -ge 128 ] || [ "$rc" = "127" ] || [ "$rc" = "134" ]; }

echo "== Guard: derin girdi TEMİZ hata vermeli (çökme değil) =="
for eng in 1 0; do
  name=$([ "$eng" = 1 ] && echo VM || echo interp)
  for kind in chain paren; do
    [ "$kind" = chain ] && gen_chain 20000 > "$TMP/deep.lk" || gen_paren 20000 > "$TMP/deep.lk"
    rc=$(run "$TMP/deep.lk" "$eng")
    if is_crash "$rc"; then echo "  FAIL [$name/$kind] ÇÖKTÜ (rc=$rc — sinyal/SIGABRT)"; fail=1
    elif [ "$rc" = "0" ]; then echo "  FAIL [$name/$kind] guard tetiklenmedi (rc=0)"; fail=1
    else echo "  OK   [$name/$kind] temiz hata (rc=$rc)"; fi
  done
done

echo "== Regresyon: normal derinlikteki ifade doğru çalışmalı =="
gen_chain 500 > "$TMP/ok.lk"   # 501 sonucu; guard'ın (1000) çok altında
for eng in 1 0; do
  name=$([ "$eng" = 1 ] && echo VM || echo interp)
  if [ "$eng" = "0" ]; then out=$(LOOK_CLI_VM=0 "$LK" "$TMP/ok.lk" 2>/dev/null); else out=$("$LK" "$TMP/ok.lk" 2>/dev/null); fi
  if [ "$out" = "501" ]; then echo "  OK   [$name] 500-terim zincir = 501"; else echo "  FAIL [$name] beklenen 501, gelen '$out'"; fail=1; fi
done
printf 'print(1+2+3*4-5)\n' > "$TMP/sm.lk"
[ "$("$LK" "$TMP/sm.lk" 2>/dev/null)" = "10" ] && echo "  OK   [VM] karışık küçük ifade = 10" || { echo "  FAIL karışık ifade"; fail=1; }

[ "$fail" = 0 ] && echo "== eval derinlik guard: TÜM TESTLER GEÇTİ ==" || echo "== eval derinlik guard: BAŞARISIZ =="
exit $fail

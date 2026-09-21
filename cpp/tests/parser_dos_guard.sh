#!/usr/bin/env bash
# LOOK dil-parser'ı DoS-guard'ı — parser adversarial turu (2026-09-21).
# LOOK'un KENDİ parser'ı (parser.cpp/lexer.cpp) TÜM motorların paylaştığı tek ön-yüz →
# bir parse bug'ı differential-KÖR (iki motor da aynı yanlış AST'yi alır). Özyinelemeli-inişli
# parser'da derin-yuvalama fiziksel stack'i taşırır; eval(untrusted) bunu SALDIRGAN-erişimli
# kılar. parser.cpp'de 3 katmanlı guard var (MAX_STMT_DEPTH=150, MAX_EXPR_DEPTH,
# MAX_BINOP_CHAIN=1000) — bu test onları CI'da pinler: her patolojik girdi, HER motorda
# (--check / VM / tree-walk) TEMİZ parse-hatası vermeli, ASLA crash/hang olmamalı.
#   Kullanım: bash tests/parser_dos_guard.sh ./build/lk
set -u
LK="${1:-./build/lk}"
command -v python3 >/dev/null 2>&1 || { echo "  (atlandı: python3 gerekli)"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0; ran=0

gen() { python3 -c "$1" > "$TMP/p.lk"; }
# Her girdiyi 3 modda koştur; her modda: crash/hang → FAIL, temiz-hata/ok → PASS.
probe() {
  local tag="$1"
  for spec in "--check|" "VM|" "tree|LOOK_CLI_VM=0"; do
    ran=$((ran+1))
    local mode="${spec%%|*}" env="${spec##*|}" flag=""
    [ "$mode" = "--check" ] && flag="--check"
    local out rc
    out=$(env $env ASAN_OPTIONS=abort_on_error=0 timeout 30 "$LK" $flag "$TMP/p.lk" 2>&1); rc=$?
    if [ $rc -eq 124 ]; then echo "  FAIL[$mode] $tag → HANG"; fails=$((fails+1))
    elif [ $rc -ge 128 ] || echo "$out" | grep -qE "stack-overflow|SEGV|Segmentation"; then
      echo "  FAIL[$mode] $tag → CRASH(rc=$rc)"; fails=$((fails+1))
    else
      echo "  ok[$mode]  $tag"
    fi
  done
}

echo "LOOK parser DoS-guard (derin-yuvalama → temiz hata, crash/hang YOK; 3 motor):"
probe "5000 parens"          "open('$TMP/p.lk','w').write('print('+'('*5000+'1'+')'*5000+')')"
probe "5000 nested arrays"   "open('$TMP/p.lk','w').write('print('+'['*5000+'1'+']'*5000+')')"
probe "5000 ** chain (right-assoc)" "open('$TMP/p.lk','w').write('print(1'+'**2'*5000+')')"
probe "20000 + chain"        "open('$TMP/p.lk','w').write('print(1'+'+1'*20000+')')"
probe "3000 open blocks"     "open('$TMP/p.lk','w').write('{'*3000)"
probe "2000 nested calls"    "open('$TMP/p.lk','w').write('f'+'('*2000+')'*2000)"

echo ""
if [ $fails -ne 0 ]; then echo "PARSER DoS-GUARD: $fails/$ran FAIL"; exit 1; fi
echo "PARSER DoS-GUARD: $ran/$ran PASS (derin-yuvalama her motorda temiz-hata, crash yok)"

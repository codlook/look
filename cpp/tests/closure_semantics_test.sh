#!/usr/bin/env bash
# closure_semantics_test.sh — closure yakalama parite guard'ı (58. bug).
#
# tree-walk (canonical) ile VM AYNI çıktıyı vermeli. 7 vaka closure semantiğinin
# tüm köşelerini kapsar. Fix ÖNCESİ B ve C AYRIŞIR (bilinen bug); fix SONRASI
# hepsi parite olmalı. A/D/E/F/G regresyon kilidi (fix bunları BOZMAMALI).
#
# NOT: differential ana suite'e EKLENMEDİ (B/C şu an ayrıştığı için kırar);
# bu ayrı guard fix'in doğrulaması + hedefi. Fix landığında ana suite'e taşınır.
set -u
LK="${1:-./build/lk}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/A.lk" <<'LK'
$v=5
$f=fn()=>$v
$v=10
print($f())
LK
cat > "$TMP/B.lk" <<'LK'
function m(){ $v=5; $f=fn()=>$v; $v=10; return $f() }
print(m())
LK
cat > "$TMP/C.lk" <<'LK'
$fns=[]
$i=0
while($i<3){ $v=$i; push($fns, fn()=>$v); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
cat > "$TMP/D.lk" <<'LK'
function m(){
  $fns=[]
  $i=0
  while($i<3){ $v=$i; push($fns, fn()=>$v); $i=$i+1 }
  return $fns[0]() . $fns[1]() . $fns[2]()
}
print(m())
LK
cat > "$TMP/E.lk" <<'LK'
$fns=[]
$i=0
while($i<3){ push($fns, fn()=>$i); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
cat > "$TMP/F.lk" <<'LK'
$v=5
$g=function(){ $v=99 }
$g()
print($v)
LK
cat > "$TMP/G.lk" <<'LK'
function m(){ $v=5; $g=function(){ $v=99 }; $g(); return $v }
print(m())
LK
# H = C2: değişken döngü DIŞINDA tanımlı → per-iter YOK, tüm closure'lar son değeri
# görür (222). Fix'in 2c-narrow koşulu (döngü-body'de tanımlı) TETİKLENMEMELİ →
# $v global kalır → parite. Bu, fix'in route/setup var'larını (döngü-dışı) bozmadığını
# kilitler (analizci güvenlik koşulu a).
cat > "$TMP/H.lk" <<'LK'
$v=99
$fns=[]
$i=0
while($i<3){ $v=$i; push($fns, fn()=>$v); $i=$i+1 }
print($fns[0]() . $fns[1]() . $fns[2]())
LK
# I = catch değişkeni yakalama (58 simetri). $err catch-var'ı declare_local'dan geçer
# ama LOAD_EXC ham değeri yazardı → boxed slot cell sanılıp capture null okurdu.
# Fix: boxed catch-var'a cell tahsis + exception cell[0]'a. Döngü-içi → per-iter →
# e0e1e2 (fix ÖNCESİ VM null null null verirdi). Cell'in atama-DIŞI bildirim yollarını kilitler.
cat > "$TMP/I.lk" <<'LK'
$fns=[]
$i=0
while($i<3){
  try { throw ("e" . $i) } catch($err) { push($fns, fn()=>$err) }
  $i=$i+1
}
print($fns[0]() . $fns[1]() . $fns[2]())
LK

for c in A B C D E F G H I; do
  tw=$(LOOK_CLI_VM=0 timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  vm=$(timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  if [ "$tw" = "$vm" ]; then
    echo "  PASS [$c] parite: $tw"
  else
    echo "  FAIL [$c] AYRISMA: tree-walk=[$tw] vm=[$vm]"; fail=1
  fi
done
[ $fail = 0 ] && echo "PASS: closure semantics (58 — 9 vaka parite (C2 + catch-var dahil))" || echo "FAIL: closure semantics (58 — B/C fix bekliyor)"
exit $fail

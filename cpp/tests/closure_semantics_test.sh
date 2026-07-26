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
# J = PARAMETRE yakalama + capture-SONRASI mutasyon (58 simetri). Param constructor'da
# declare_local'ı atlar → boxed_slots'a girmez, cell tahsis edilmez → capture snapshot
# okur. closure ÖNCE kurulup SONRA mutasyon → tw by-ref 105, fix ÖNCESİ vm snapshot 5.
# (Mutasyon-ÖNCE-capture bug'ı MASKELER: snapshot=mutasyonlu değer — bu yüzden mutasyon
# SONRASI test şart.) Fix: boxed param prologue'da cell'e kutulanır.
cat > "$TMP/J.lk" <<'LK'
function t($p){ $f=fn()=>$p; $p=$p+100; return $f() }
print(t(5))
LK
# K = FOREACH değişkeni yakalama + capture-SONRASI mutasyon (58 simetri). foreach var'ı
# manuel kayıtla (locals_.push_back) declare_local'ı atlar → cell yok. Fix: boxed loop
# var ayrı cell-slot, FOR_STEP sonrası her iter taze cell. tw=101,102; fix ÖNCESİ vm=1,2.
cat > "$TMP/K.lk" <<'LK'
$fns=[]
foreach([1,2] as $v){ push($fns, fn()=>$v); $v=$v+100 }
print($fns[0]() . "," . $fns[1]())
LK

# L = NAMED nested fonksiyon dıştaki değişkeni yakalar + capture-sonrası mutasyon.
# compile_func_decl MAKE_CLOSURE'u capture kurulumu OLMADAN emit ediyordu → proto'daki
# LOAD_CAPTURE runtime'da "Capture index dışı" ile ÇÖKÜYORDU (VM crash, tw çalışırken).
# Fix: compile_closure'ın capture yükleme bloğu compile_func_decl'e de eklendi. 17 parite
# (fix ÖNCESİ vm crash). Anonim closure DEĞİL named-fn capture yolunu kilitler.
cat > "$TMP/L.lk" <<'LK'
function outer($p){ function inner(){ return $p } $p=$p+7; return inner() }
print(outer(10))
LK

for c in A B C D E F G H I J K L; do
  tw=$(LOOK_CLI_VM=0 timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  vm=$(timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  if [ "$tw" = "$vm" ]; then
    echo "  PASS [$c] parite: $tw"
  else
    echo "  FAIL [$c] AYRISMA: tree-walk=[$tw] vm=[$vm]"; fail=1
  fi
done
[ $fail = 0 ] && echo "PASS: closure semantics (58 — 12 vaka parite (C2+catch+param+foreach+named))" || echo "FAIL: closure semantics (58 — B/C fix bekliyor)"
exit $fail

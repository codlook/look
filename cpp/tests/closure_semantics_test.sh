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

for c in A B C D E F G; do
  tw=$(LOOK_CLI_VM=0 timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  vm=$(timeout 10 "$LK" "$TMP/$c.lk" 2>&1 | grep -v "INFO\|Pool" | tr '\n' ' ' | tr -s ' ')
  if [ "$tw" = "$vm" ]; then
    echo "  PASS [$c] parite: $tw"
  else
    echo "  FAIL [$c] AYRISMA: tree-walk=[$tw] vm=[$vm]"; fail=1
  fi
done
[ $fail = 0 ] && echo "PASS: closure semantics (58 — 7 vaka parite)" || echo "FAIL: closure semantics (58 — B/C fix bekliyor)"
exit $fail

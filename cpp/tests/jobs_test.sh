#!/bin/bash
# jobs:: guard'i — kuyruk kaliciligi ve ÇOK SÜREÇLİ claim yarisi.
#
# NEDEN: jobs_stdlib.cpp (457 satir) guard'siz bir yuzeydi ve claim yolu
# SELECT+UPDATE seklinde, ARADA transaction/durum kontrolu OLMADAN yaziliydi.
# Aradaki std::lock_guard yalnizca SUREC ICI mutex — FastCGI multi-worker'da
# her worker AYRI SUREC oldugu icin hicbir koruma saglamiyordu.
# OLCULDU (duzeltme oncesi): 4 surec / 40 is -> 34 claim, 29 tekil
#   => 5 is BIRDEN FAZLA surece verildi (ayni e-posta iki kez, ayni odeme iki kez).
# Cozum: tek ifadelik atomik claim (UPDATE ... WHERE id=(SELECT ...) AND
# status='pending' RETURNING ...) + WAL + busy_timeout.
#
# Kullanim: bash jobs_test.sh <lk_yolu>
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
cd "$TMP" || exit 1

cat > push.lk <<'LK'
use jobs
$i = 0
while ($i < 40) { jobs::push("q", "is-" . $i); $i = $i + 1 }
print("ok")
LK
cat > claim.lk <<'LK'
use jobs
$s = ""
$i = 0
while ($i < 30) {
  $j = jobs::next("q")
  if ($j == null) { break }
  $s = $s . $j.id . " "
  $i = $i + 1
}
print($s)
LK
cat > akis.lk <<'LK'
use jobs
# retry -> max_retries dolunca failed (sonsuz denenmemeli)
jobs::push("r", "x")
$t = 0
while ($t < 8) { $j = jobs::next("r"); if ($j == null) { break }; jobs::fail($j.id); $t = $t + 1 }
$rs = jobs::stats("r")
# run_after: 4. parametre GECIKME (3. parametre max_retries — kolay karistirilir)
jobs::push("d", "gec", 3, 3600)
$erken = jobs::next("d")
# asili is: claim edilip done/fail denmezse recover geri almali
jobs::push("s", "asili")
jobs::next("s")
$kurt = jobs::recover("s", 0)
$tekrar = jobs::next("s")
# done edilen is tekrar alinmamali
jobs::push("o", "bir")
$o = jobs::next("o")
jobs::done($o.id)
$o2 = jobs::next("o")
print($rs.failed . "|" . ($erken == null ? "gec" : "ERKEN") . "|" . $kurt . "|" .
      ($tekrar != null ? "kurtarildi" : "YOK") . "|" . ($o2 == null ? "bitti" : "TEKRAR"))
LK

export JOBS_DB="$TMP/jobs.db"
"$LK" push.lk >/dev/null 2>&1
for i in 1 2 3 4; do "$LK" claim.lk > "p$i.txt" 2>/dev/null & done
wait
cat p*.txt | grep -v '^\[' | tr -s ' ' '\n' | grep '[0-9]' | sort -n > all.txt
toplam=$(wc -l < all.txt); tekil=$(sort -u all.txt | wc -l)
if [ "$toplam" = "$tekil" ] && [ "$tekil" -gt 0 ]; then
  echo "  PASS cok-surecli claim: $toplam claim / $tekil tekil (cift isleme yok)"
else
  echo "  FAIL CIFT ISLEME: $toplam claim ama $tekil tekil is"
  echo "       -> claim atomik degil (SELECT+UPDATE arasi yaris; surec ici mutex yetmez)"
  fail=1
fi
if [ "$tekil" -ne 40 ]; then
  echo "  FAIL is KAYBI: 40 isten $tekil tanesi alinabildi"; fail=1
fi

export JOBS_DB="$TMP/jobs2.db"
out=$("$LK" akis.lk 2>&1 | grep -v '^\[' | tail -1)
bek="1|gec|1|kurtarildi|bitti"
if [ "$out" = "$bek" ]; then
  echo "  PASS akis: retry->failed, run_after, recover, done"
else
  echo "  FAIL jobs akisi: [$out] (beklenen $bek)"
  echo "       sira: failed-sayisi|gecikmeli-is|recover-sayisi|kurtarilan-alindi|done-tekrar-alinmadi"
  fail=1
fi

# SOGUK ACILIS: DB HIC YOKKEN N surec ayni anda schema olusturur.
# busy_timeout sqlite3_open'dan HEMEN SONRA ayarlanmazsa, WAL gecisi ve
# create_schema() ozel kilit isterken aninda SQLITE_BUSY doner ve surecler
# 'schema hatasi: database is locked' ile COKER. Daha kotusu: olen surecler
# claim yarisini MASKELER (az surec kalinca yaris gorunmez).
rm -f "$TMP/cold.db"*
export JOBS_DB="$TMP/cold.db"
cat > cold.lk <<'LK'
use jobs
$j = jobs::next("q")
print("ok")
LK
for i in 1 2 3 4 5 6; do "$LK" cold.lk > "c$i.out" 2> "c$i.err" & done
wait
kilit=0
for i in 1 2 3 4 5 6; do grep -qi "locked" "c$i.err" 2>/dev/null && kilit=$((kilit+1)); done
if [ "$kilit" = "0" ]; then
  echo "  PASS soguk acilis: 6 surec es zamanli schema olusturdu, kilit hatasi yok"
else
  echo "  FAIL soguk acilis: 6 surecten $kilit tanesi 'database is locked' ile coktu"
  echo "       -> sqlite3_busy_timeout() sqlite3_open'dan HEMEN SONRA cagrilmali (WAL/schema oncesi)"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: jobs::" || echo "FAIL: jobs::"
exit $fail

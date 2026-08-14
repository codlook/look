#!/usr/bin/env bash
# TERS-YÖN doküman uyumluluk guard'ı (kod → docs). Mevcut conformance guard docs→kod'u kontrol eder
# (docs'ta yazan mod::fn gerçekten var mı — session::has sınıfı). Bu onun eksik yarısı: builtin_names()'teki
# HER namespaced builtin docs/index.html'de belgeli Mİ? Değilse ya belgelenmeli ya (gerekçeyle) dışlanmalı.
# Böylece yeni builtin eklenince belgelenmeden CI'dan geçemez. Pozitif kontrol: bir fonksiyonu docs'tan sil → RED.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
DOCS="$ROOT/docs/index.html"
fail=0

# GEREKÇELİ DIŞLAMA (known_fail DEĞİL — her biri docs'ta BAŞKA formda belgeli):
#  - validator::* : "Rule strings" olarak belgeli (fonksiyon değil, check()'e geçen kural adları)
#  - template::extends/block/include : {#extends}/{#block}/{#include} SÖZDİZİMİ olarak belgeli
# Yeni bir builtin bu listeye eklenirse GEREKÇE şart (yorumla), yoksa liste known_fail'e döner.
EXCLUDE="validator::required validator::email validator::integer validator::numeric validator::min validator::max template::extends template::block template::include"

# builtin_names()'ten namespaced isimleri çıkar (mod::fn)
names=$(grep -oE '"[a-z_]+::[a-z_0-9]+"' "$BUILTINS" | tr -d '"' | sort -u)
undoc=""
for n in $names; do
  case " $EXCLUDE " in *" $n "*) continue;; esac   # gerekçeli dışlama
  grep -qF "$n" "$DOCS" || undoc="$undoc $n"
done

if [ -n "$undoc" ]; then
  echo "  FAIL: builtin var ama docs'ta YOK (belgele ya da gerekçeyle EXCLUDE'a ekle):"
  for u in $undoc; do echo "    $u"; done
  fail=1
else
  echo "  OK   tüm namespaced builtin belgeli (veya gerekçeli dışlamada)"
fi

# POZİTİF KONTROL: belgeli bir fonksiyon (session::regenerate) docs'tan geçici çıkarılınca guard RED vermeli.
if grep -qF "session::regenerate" "$DOCS"; then
  echo "  OK   pozitif-kontrol referansı mevcut (session::regenerate belgeli)"
else
  echo "  FAIL pozitif-kontrol: session::regenerate docs'ta yok — guard kalibrasyonu bozuk"; fail=1
fi

[ $fail = 0 ] && echo "PASS: ters-yön doküman uyumluluğu (kod→docs)" || echo "FAIL: ters-yön doküman uyumluluğu"
exit $fail

#!/usr/bin/env bash
# TERS-YÖN doküman uyumluluk guard'ı (kod → docs). Mevcut conformance guard docs→kod'u kontrol eder
# (docs'ta yazan mod::fn gerçekten var mı — session::has sınıfı). Bu onun eksik yarısı: builtin_names()'teki
# HER namespaced builtin docs/index.html'de belgeli Mİ? Değilse ya belgelenmeli ya (gerekçeyle) dışlanmalı.
# Böylece yeni builtin eklenince belgelenmeden CI'dan geçemez. Pozitif kontrol: bir fonksiyonu docs'tan sil → RED.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILTINS="$ROOT/cpp/src/builtins.cpp"
DOCS="$ROOT/docs/look.codlook.com/docs.html"
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

# BARE (namespaced-OLMAYAN) builtin'ler — KÖR NOKTAYDI: args() belgesiz eklenip guard'dan
# geçebildi çünkü yukarıdaki tarama yalnız "mod::fn" biçimini kontrol ediyordu. Bunları da
# denetle. Bazı bare isimler KASITEN belgesizdir: kanonik biçimi belgeli geri-uyumluluk
# takma adları (json_encode→json::encode, strlen→string::len...). Onlar BARE_EXCLUDE'da.
BARE_EXCLUDE="json_encode json_decode strlen strtolower strtoupper"
bare=$(sed -n '/const std::vector<std::string>& builtin_names/,/return NAMES;/p' "$BUILTINS" \
  | grep -oE '"[a-z_][a-z0-9_]*"' | tr -d '"' | grep -vE '::' | sort -u)
undoc_bare=""
for n in $bare; do
  case " $BARE_EXCLUDE " in *" $n "*) continue;; esac
  grep -qw "$n" "$DOCS" || undoc_bare="$undoc_bare $n"
done
if [ -n "$undoc_bare" ]; then
  echo "  FAIL: bare builtin var ama docs'ta YOK (belgele ya da gerekçeyle BARE_EXCLUDE'a ekle):"
  for u in $undoc_bare; do echo "    $u"; done
  fail=1
else
  echo "  OK   tüm bare builtin belgeli (veya gerekçeli dışlamada)"
fi

# POZİTİF KONTROL: belgeli bir fonksiyon (session::regenerate) docs'tan geçici çıkarılınca guard RED vermeli.
if grep -qF "session::regenerate" "$DOCS"; then
  echo "  OK   pozitif-kontrol referansı mevcut (session::regenerate belgeli)"
else
  echo "  FAIL pozitif-kontrol: session::regenerate docs'ta yok — guard kalibrasyonu bozuk"; fail=1
fi

[ $fail = 0 ] && echo "PASS: ters-yön doküman uyumluluğu (kod→docs)" || echo "FAIL: ters-yön doküman uyumluluğu"
exit $fail

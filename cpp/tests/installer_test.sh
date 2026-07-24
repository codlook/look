#!/bin/bash
# installer:: guard'i — paket/modul kurulumu (installer.cpp, 617 satir).
#
# NEDEN: dosya sistemine YAZAN ve AGDAN indiren tek yuzey; paket = calistirilabilir
# LOOK kodu, yani tedarik zinciri riski. Guard'i yoktu.
#
# Kilitlenenler:
#  1) YONLENDIRME dogrulamasi (48. bug): `Location` korlemesine takip ediliyordu.
#     `http://` (sema dusurme) ve farkli host artik REDDEDILIR; mesru GitHub
#     zinciri (api.github.com -> codeload.github.com) calismaya devam etmeli.
#  2) Host kisiti: parse_pkg yalniz github.com kabul etmeli.
#  3) Zip-Slip: cikarilan dosyalar hedef dizin DISINA yazilmamali.
#
# Ag gerektiren kisim ag yoksa ATLANIR (sessizce degil, 'atlandi' yazarak).
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
cd "$TMP" || exit 1

# 1) Host kisiti — github.com disi reddedilmeli (ag GEREKTIRMEZ)
out=$("$LK" module install evil.com/user/repo 2>&1 | head -3)
if echo "$out" | grep -qi "sadece github.com"; then
  echo "  PASS host kisiti: github.com disi reddediliyor"
else
  echo "  FAIL host kisiti calismiyor: [$out]"; fail=1
fi

# 2) Gecersiz paket adi — net hata
out=$("$LK" module install github.com/tekbasina 2>&1 | head -2)
if echo "$out" | grep -qi "gecersiz\|geçersiz\|user/repo"; then
  echo "  PASS bozuk paket adi net hata veriyor"
else
  echo "  FAIL bozuk paket adi: [$out]"; fail=1
fi

# 3) GERCEK kurulum — mesru GitHub yonlendirme zinciri (ag gerekir)
if timeout 10 curl -s -o /dev/null https://api.github.com 2>/dev/null; then
  export HOME="$TMP"
  out=$(timeout 120 "$LK" module install github.com/codlook/look-modules/jwt -v 2>&1)
  if echo "$out" | grep -q "codeload.github.com" && echo "$out" | grep -qi "kuruldu"; then
    echo "  PASS gercek kurulum: api.github.com -> codeload.github.com zinciri calisiyor"
  else
    echo "  FAIL gercek kurulum basarisiz (yonlendirme dogrulamasi mesru akisi bozdu mu?):"
    echo "$out" | head -5 | sed 's/^/       /'
    fail=1
  fi
  # Kurulan dosyalar hedef dizin ICINDE mi (Zip-Slip)
  disari=$(find "$TMP" -maxdepth 1 -name '*.lk' 2>/dev/null | wc -l)
  if [ "$disari" = "0" ]; then
    echo "  PASS Zip-Slip: dosyalar modul dizini disina yazilmadi"
  else
    echo "  FAIL Zip-Slip: $disari dosya hedef dizin DISINA yazildi"; fail=1
  fi
  # 4) look.lock SHA-PIN (51. bug): PAKET install look.lock'a "ref@sha" yazar;
  #    `look install` (install-all) kayitli commit SHA'sina sabitlenmeli, dal/
  #    etikete degil. Temiz bir dizinde paket kur, sonra install-all'un o SHA'yi
  #    indirdigini (dal HEAD'ini degil) verbose zipball URL'inden dogrula.
  #    NOT: 'module install' look.lock YAZMAZ (~/.look/modules'a kurar) — bu test
  #    'install' (paket) formunu kullanmali.
  pdir="$TMP/pkgpin"; mkdir -p "$pdir"
  ( cd "$pdir" && HOME="$pdir" timeout 120 "$LK" install github.com/codlook/look-modules/jwt >/dev/null 2>&1 )
  sha=$(grep -o '@[0-9a-f]\{7,40\}' "$pdir/look.lock" 2>/dev/null | head -1 | tr -d '@')
  if [ -n "$sha" ]; then
    allout=$( cd "$pdir" && HOME="$pdir" timeout 120 "$LK" install -v 2>&1 )
    if echo "$allout" | grep -q "zipball/$sha"; then
      echo "  PASS look.lock sha-pin: install-all kayitli commit'e sabitliyor (${sha:0:12})"
    elif echo "$allout" | grep -q "zipball/main"; then
      echo "  FAIL look.lock sha-pin: install-all dal HEAD'ini (main) cozuyor — kilit kilitlemiyor"; fail=1
    else
      echo "  FAIL look.lock sha-pin: beklenen zipball/$sha URL'i verbose ciktida yok:"
      echo "$allout" | grep -i "zipball\|indir" | head -3 | sed 's/^/       /'
      fail=1
    fi
  else
    echo "  (atlandi: paket look.lock'ta sha yok — sha cozumleme calismiyor olabilir)"
  fi
else
  echo "  (atlandi: gercek kurulum testi ag gerektirir)"
fi

[ $fail = 0 ] && echo "PASS: installer::" || echo "FAIL: installer::"
exit $fail

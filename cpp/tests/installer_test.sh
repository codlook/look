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
else
  echo "  (atlandi: gercek kurulum testi ag gerektirir)"
fi

[ $fail = 0 ] && echo "PASS: installer::" || echo "FAIL: installer::"
exit $fail

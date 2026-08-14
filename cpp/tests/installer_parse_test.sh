#!/usr/bin/env bash
# installer:: parse-katmanı guard'ı — AĞSIZ, CI-koşabilir. parse_pkg (installer.cpp:101-174)
# tüm bu redleri İNDİRMEDEN ÖNCE yapar → network YOK. (Zip-slip/redirect gerçek indirme/
# 3xx ister → C++ unit-test hedefli-takip; tam installer_test.sh ağlı, manuel kalır.)
# Kapsam: host kısıtı (yalnız github.com) · geçersiz ad · path-traversal (user + subdir).
# fail-loud + pozitif kontrol: meşru ad reddedilMEmeli (aksi halde red her şeyi reddeder).
set -u
LK="${1:-./build/lk}"
fail=0

expect_reject() {  # $1=etiket $2=paket $3=beklenen-mesaj-parçası
  local out; out=$("$LK" module install "$2" 2>&1 | head -2)
  if echo "$out" | grep -qi "$3"; then echo "  OK $1: reddedildi"
  else echo "  FAIL $1: red beklendi ('$3'), gelen: [$out]"; fail=1; fi
}

# NOT: parçalar installer.cpp'nin İNGİLİZCE runtime string'leriyle eşleşir (dil politikası).
expect_reject "host kısıtı (github.com dışı)"  "evil.com/user/repo"                 "Only github.com"
expect_reject "geçersiz ad (tek bileşen)"       "github.com/tekbasina"               "user/repo required\|Invalid package"
expect_reject "traversal (user bileşeni)"        "github.com/../../etc/passwd"        "path-escape"
expect_reject "traversal (subdir bileşeni)"      "github.com/user/repo/../../../evil" "path-escape"

# POZİTİF KONTROL (Kural 1): meşru paket adı parse'ı GEÇMELİ (red aşamasını değil, indirme/404'ü
# görmeli). Aksi halde parse her şeyi reddediyordur → yukarıdaki redler anlamsız (yanlış-yeşil).
out=$("$LK" module install "github.com/codlook/look-modules/jwt" 2>&1 | head -3)
if echo "$out" | grep -qiE "path-escape|Only github.com|Invalid package"; then
  echo "  FAIL pozitif-kontrol: meşru ad parse'ta REDDEDİLDİ (parse aşırı-red): [$out]"; fail=1
else
  echo "  OK pozitif-kontrol: meşru ad parse'ı geçti (indirme/404 aşamasına ulaştı)"
fi

[ $fail = 0 ] && echo "PASS: installer:: parse-katmanı (host/ad/traversal)" || echo "FAIL: installer:: parse-katmanı"
exit $fail

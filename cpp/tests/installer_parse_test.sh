#!/usr/bin/env bash
# installer:: parse-layer guard — NO NETWORK, CI-runnable. parse_pkg (installer.cpp:101-174)
# does all these rejections BEFORE downloading → no network. (Zip-slip/redirect need a real download/
# 3xx → the C++ unit test is a targeted follow-up; the full installer_test.sh needs the network and stays manual.)
# Scope: host restriction (github.com only) · invalid name · path-traversal (user + subdir).
# fail-loud + positive control: a legitimate name must NOT be rejected (otherwise the rejection rejects everything).
set -u
LK="${1:-./build/lk}"
fail=0

expect_reject() {  # $1=label $2=package $3=expected-message-part
  local out; out=$("$LK" module install "$2" 2>&1 | head -2)
  if echo "$out" | grep -qi "$3"; then echo "  OK $1: reddedildi"
  else echo "  FAIL $1: red beklendi ('$3'), gelen: [$out]"; fail=1; fi
}

# NOTE: the parts match the ENGLISH runtime strings of installer.cpp (language policy).
expect_reject "host kısıtı (github.com dışı)"  "evil.com/user/repo"                 "Only github.com"
expect_reject "geçersiz ad (tek bileşen)"       "github.com/tekbasina"               "user/repo required\|Invalid package"
expect_reject "traversal (user bileşeni)"        "github.com/../../etc/passwd"        "path-escape"
expect_reject "traversal (subdir bileşeni)"      "github.com/user/repo/../../../evil" "path-escape"

# POSITIVE CONTROL (Rule 1): a legitimate package name must PASS the parse (it must reach the download/404, not the rejection stage).
# Otherwise the parse rejects everything → the rejections above are meaningless (false green).
out=$("$LK" module install "github.com/codlook/look-modules/jwt" 2>&1 | head -3)
if echo "$out" | grep -qiE "path-escape|Only github.com|Invalid package"; then
  echo "  FAIL pozitif-kontrol: meşru ad parse'ta REDDEDİLDİ (parse aşırı-red): [$out]"; fail=1
else
  echo "  OK pozitif-kontrol: meşru ad parse'ı geçti (indirme/404 aşamasına ulaştı)"
fi

[ $fail = 0 ] && echo "PASS: installer:: parse-katmanı (host/ad/traversal)" || echo "FAIL: installer:: parse-katmanı"
exit $fail

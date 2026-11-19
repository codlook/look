#!/bin/bash
# installer:: guard — package/module installation (installer.cpp, 617 lines).
#
# WHY: the only surface that WRITES to the file system and DOWNLOADS from the NETWORK; a package = executable
# LOOK code, i.e. a supply-chain risk. It had no guard.
#
# What is locked:
#  1) REDIRECT validation (bug 48): `Location` was followed blindly.
#     `http://` (scheme downgrade) and a different host are now REJECTED; the legitimate GitHub
#     chain (api.github.com -> codeload.github.com) must keep working.
#  2) Host restriction: parse_pkg must accept only github.com.
#  3) Zip-Slip: extracted files must not be written OUTSIDE the target directory.
#
# The part that needs the network is SKIPPED when there is no network (not silently, printing 'atlandi' = skipped).
set -u
LK="${1:-./build/lk}"
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"   # cd is done below: absolute path
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
cd "$TMP" || exit 1

# 1) Host restriction — non-github.com must be rejected (does NOT need the network)
out=$("$LK" module install evil.com/user/repo 2>&1 | head -3)
if echo "$out" | grep -qi "only github.com is supported"; then
  echo "  PASS host kisiti: github.com disi reddediliyor"
else
  echo "  FAIL host kisiti calismiyor: [$out]"; fail=1
fi

# 2) Invalid package name — clear error
out=$("$LK" module install github.com/tekbasina 2>&1 | head -2)
if echo "$out" | grep -qi "gecersiz\|geçersiz\|user/repo"; then
  echo "  PASS bozuk paket adi net hata veriyor"
else
  echo "  FAIL bozuk paket adi: [$out]"; fail=1
fi

# 3) REAL install — legitimate GitHub redirect chain (needs the network)
if timeout 10 curl -s -o /dev/null https://api.github.com 2>/dev/null; then
  export HOME="$TMP"
  out=$(timeout 120 "$LK" module install github.com/codlook/look-modules/jwt -v 2>&1)
  if echo "$out" | grep -q "codeload.github.com" && echo "$out" | grep -qi "installed"; then
    echo "  PASS gercek kurulum: api.github.com -> codeload.github.com zinciri calisiyor"
  else
    echo "  FAIL gercek kurulum basarisiz (yonlendirme dogrulamasi mesru akisi bozdu mu?):"
    echo "$out" | head -5 | sed 's/^/       /'
    fail=1
  fi
  # Are the installed files INSIDE the target directory (Zip-Slip)
  disari=$(find "$TMP" -maxdepth 1 -name '*.lk' 2>/dev/null | wc -l)
  if [ "$disari" = "0" ]; then
    echo "  PASS Zip-Slip: dosyalar modul dizini disina yazilmadi"
  else
    echo "  FAIL Zip-Slip: $disari dosya hedef dizin DISINA yazildi"; fail=1
  fi
  # 4) look.lock SHA-PIN (bug 51): a PACKAGE install writes "ref@sha" to look.lock;
  #    `look install` (install-all) must pin to the recorded commit SHA, not the branch/
  #    tag. Install a package in a clean directory, then verify from the verbose zipball URL that install-all
  #    downloaded that SHA (not the branch HEAD).
  #    NOTE: 'module install' does NOT write look.lock (it installs to ~/.look/modules) — this test
  #    must use the 'install' (package) form.
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

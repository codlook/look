#!/bin/bash
# caching_sha2_password CI guard'i — GERCEK mysql:8'e karsi (GitHub Actions service).
#
# NEDEN AYRI DOSYA: mysql_auth_test.sh docker konteynerlerini kendi kuruyor (my8/my57
# surum matrisi) ve CI-disi. Bu kosucu ONUN aksine TEK bir mysql:8 servisine karsi
# kosar (Actions `services: mysql:` health-check'li) — caching_sha2_password auth
# yolu boylece HER PUSH'ta, gercek MySQL 8 protokolune karsi dogrulanir.
#
# Sunucu, caching_sha2_password kullanan bir kullaniciya ihtiyac duyar. MySQL 8
# VARSAYILANI budur; MYSQL_ROOT_PASSWORD ile olusan `root` zaten caching_sha2'dir.
# Ama root yalniz `localhost` (unix soket) icin caching_sha2 host-eslesmesi
# yapabilir; TCP icin ayri bir kullanici olustururuz (asagidaki SQL, CI job'inda).
#
# Kullanim:
#   MYDSN=mysql://ci:sifre123@127.0.0.1:3306/testdb \
#   MYUSER=ci MYHOST=127.0.0.1 MYPORT=3306 MYDB=testdb \
#   bash mysql_caching_sha2_ci.sh <lk_yolu>
set -u
LK="${1:-./build/lk}"
: "${MYDSN:?MYDSN gerekli (ornek: mysql://ci:sifre123@127.0.0.1:3306/testdb)}"

DIR="$(cd "$(dirname "$0")" && pwd)"
out="$(timeout 60 "$LK" "$DIR/mysql_caching_sha2_ci.lk" 2>&1)"
echo "$out"

ok="$(echo "$out"   | grep -E '^AUTH_OK\|'     | head -1)"
rej="$(echo "$out"  | grep -E '^AUTH_REJECT$'  | head -1)"

fail=0

# ── EXPECTED-pin: dogru kimlik + utf8 roundtrip ─────────────────────────────────
case "$ok" in
  AUTH_OK\|8.*\|şğü-caching)
    echo "  PASS caching_sha2 roundtrip (MySQL 8, TAM + HIZLI yol): $ok" ;;
  AUTH_OK\|*)
    echo "  FAIL beklenen surum/deger tutmadi (bek: 8.x + 'şğü-caching'): $ok"; fail=1 ;;
  *)
    echo "  FAIL caching_sha2 baglanti/roundtrip basarisiz"
    echo "       -> caching_sha2_password auth yolu (scramble/RSA/cleartext) kirik olabilir"
    fail=1 ;;
esac

# ── POZITIF KONTROL: yanlis sifre reddedildi mi? ────────────────────────────────
if [ "$rej" = "AUTH_REJECT" ]; then
  echo "  PASS pozitif kontrol: yanlis sifre REDDEDILDI (auth gercekten dogruluyor)"
else
  echo "  FAIL pozitif kontrol: yanlis sifre reddedilmedi -> auth-yolu vacuous"
  echo "       (test dogru kimligi gecse bile scramble/RSA adimini atlyor olabilir)"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: caching_sha2_password CI guard temiz" || echo "FAIL: caching_sha2_password CI guard"
exit $fail

#!/bin/bash
# caching_sha2_password CI guard — against a REAL mysql:8 (GitHub Actions service).
#
# WHY A SEPARATE FILE: mysql_auth_test.sh sets up its own docker containers (my8/my57
# version matrix) and is not for CI. Unlike it, this runner runs against a SINGLE mysql:8 service
# (Actions `services: mysql:` with a health check) — so the caching_sha2_password auth
# path is verified on EVERY PUSH against the real MySQL 8 protocol.
#
# The server needs a user that uses caching_sha2_password. This is the MySQL 8
# DEFAULT; the `root` created by MYSQL_ROOT_PASSWORD is already caching_sha2.
# But root can only do a caching_sha2 host match for `localhost` (unix socket); for TCP we
# create a separate user (the SQL below, in the CI job).
#
# Usage:
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

# ── EXPECTED-pin: correct credentials + utf8 roundtrip ─────────────────────────
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

# ── POSITIVE CONTROL: was the wrong password rejected? ─────────────────────────
if [ "$rej" = "AUTH_REJECT" ]; then
  echo "  PASS pozitif kontrol: yanlis sifre REDDEDILDI (auth gercekten dogruluyor)"
else
  echo "  FAIL pozitif kontrol: yanlis sifre reddedilmedi -> auth-yolu vacuous"
  echo "       (test dogru kimligi gecse bile scramble/RSA adimini atlyor olabilir)"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: caching_sha2_password CI guard temiz" || echo "FAIL: caching_sha2_password CI guard"
exit $fail

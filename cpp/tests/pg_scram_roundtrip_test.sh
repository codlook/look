#!/usr/bin/env bash
# PG SCRAM-SHA-256 canlı roundtrip harness (VPS-only; gerçek PostgreSQL 10+ gerektirir).
# Geçici scram-sha-256 rolü+db+hba kuralı oluşturur, lk ile SCRAM login roundtrip'i koşar,
# HER DURUMDA pg_hba'yı geri alır (trap). Pozitif-kontrol: pg_hba scram-sha-256 zorlar → md5'e
# düşemez; parola/crypto kırıksa auth reddedilir ve "4242" basılmaz.
set -u
LK="${1:?usage: _pg_scram_roundtrip.sh <path-to-lk> <path-to-script.lk>}"
SCRIPT="${2:?script.lk gerekli}"
PGDATA="$(find /var/lib/pgsql -name postgresql.conf 2>/dev/null | head -1 | xargs dirname)"
HBA="$PGDATA/pg_hba.conf"
DB=looktestdb_scram
ROLE=looktest_scram
PASS=testpass_scram_2026

# postgres süperuser'a peer-auth ile (login-shell/banner YOK) eriş.
# -d template1: pg_hba ilk kuralı "local samegroup all password" postgres→db-postgres'i
# (samerole) yakalayıp parola ister; template1'e bağlanınca eşleşmez → "local all all peer" → OK.
pg() { runuser -u postgres -- psql -d template1 "$@"; }
reload() {
  runuser -u postgres -- pg_ctl reload -D "$PGDATA" >/dev/null 2>&1 \
    || systemctl reload postgresql >/dev/null 2>&1 || true
}

restore() {
  if [ -f "$HBA.scrambak" ]; then
    cp -f "$HBA.scrambak" "$HBA" && rm -f "$HBA.scrambak"; reload
  fi
  pg -q -c "DROP DATABASE IF EXISTS $DB;" >/dev/null 2>&1
  pg -q -c "DROP ROLE IF EXISTS $ROLE;"   >/dev/null 2>&1
}
trap restore EXIT

# 1) scram-sha-256 rol + db (session-scope password_encryption → parola scram-hash'lenir)
pg -q -v ON_ERROR_STOP=1 \
   -c "DROP DATABASE IF EXISTS $DB;" \
   -c "DROP ROLE IF EXISTS $ROLE;" \
   -c "SET password_encryption = 'scram-sha-256';" \
   -c "CREATE ROLE $ROLE LOGIN PASSWORD '$PASS';" \
   -c "CREATE DATABASE $DB OWNER $ROLE;" || { echo 'FAIL: rol/db kurulamadı'; exit 1; }

# rolün gerçekten SCRAM-hash'li olduğunu doğrula (md5 değil) — pozitif-kontrol ön-koşulu
KIND=$(pg -tAqc "SELECT CASE WHEN rolpassword LIKE 'SCRAM-SHA-256%' THEN 'scram' ELSE substr(rolpassword,1,4) END FROM pg_authid WHERE rolname='$ROLE';" 2>/dev/null | tr -d '[:space:]')
echo "rol parola tipi: $KIND"
[ "$KIND" = "scram" ] || { echo "FAIL: rol scram-hash'li değil ($KIND) — server SCRAM zorlamayacak"; exit 1; }

# 2) pg_hba: 127.0.0.1 için $DB/$ROLE'e scram-sha-256 zorla (en üste ekle)
cp -f "$HBA" "$HBA.scrambak"
printf 'host    %s    %s    127.0.0.1/32    scram-sha-256\n' "$DB" "$ROLE" > "$HBA.new"
cat "$HBA" >> "$HBA.new"
mv -f "$HBA.new" "$HBA"
reload
sleep 1

# 3) SCRAM roundtrip: lk scriptini koş (127.0.0.1 → scram-sha-256 dalı zorunlu)
echo '=== SCRAM roundtrip (lk) ==='
OUT=$("$LK" "$SCRIPT" 2>&1)
echo "lk çıktı: $OUT"
if echo "$OUT" | grep -q '^4242$'; then
  echo 'PASS: SCRAM-SHA-256 login roundtrip BAŞARILI (look:: birleştirilmiş crypto ile)'
  exit 0
else
  echo 'FAIL: SCRAM login roundtrip başarısız (auth reddi veya yanlış sonuç)'
  exit 1
fi

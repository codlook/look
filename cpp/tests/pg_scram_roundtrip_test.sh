#!/usr/bin/env bash
# PG SCRAM-SHA-256 live roundtrip harness (VPS-only; needs a real PostgreSQL 10+).
# Creates a temporary scram-sha-256 role+db+hba rule, runs a SCRAM login roundtrip with lk,
# restores pg_hba IN EVERY CASE (trap). Positive control: pg_hba forces scram-sha-256 → it cannot
# fall to md5; if the password/crypto is broken auth is rejected and "4242" is not printed.
set -u
LK="${1:?usage: _pg_scram_roundtrip.sh <path-to-lk> <path-to-script.lk>}"
SCRIPT="${2:?script.lk required}"
PGDATA="$(find /var/lib/pgsql -name postgresql.conf 2>/dev/null | head -1 | xargs dirname)"
HBA="$PGDATA/pg_hba.conf"
DB=looktestdb_scram
ROLE=looktest_scram
PASS=testpass_scram_2026

# access the postgres superuser via peer-auth (NO login-shell/banner).
# -d template1: the first pg_hba rule "local samegroup all password" catches postgres→db-postgres
# (samerole) and asks for a password; connecting to template1 does not match → "local all all peer" → OK.
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

# 1) scram-sha-256 role + db (session-scope password_encryption → the password is scram-hashed)
pg -q -v ON_ERROR_STOP=1 \
   -c "DROP DATABASE IF EXISTS $DB;" \
   -c "DROP ROLE IF EXISTS $ROLE;" \
   -c "SET password_encryption = 'scram-sha-256';" \
   -c "CREATE ROLE $ROLE LOGIN PASSWORD '$PASS';" \
   -c "CREATE DATABASE $DB OWNER $ROLE;" || { echo 'FAIL: could not set up the role/db'; exit 1; }

# verify that the role is really SCRAM-hashed (not md5) — precondition of the positive control
KIND=$(pg -tAqc "SELECT CASE WHEN rolpassword LIKE 'SCRAM-SHA-256%' THEN 'scram' ELSE substr(rolpassword,1,4) END FROM pg_authid WHERE rolname='$ROLE';" 2>/dev/null | tr -d '[:space:]')
echo "role password type: $KIND"
[ "$KIND" = "scram" ] || { echo "FAIL: role is not scram-hashed ($KIND) — the server will not enforce SCRAM"; exit 1; }

# 2) pg_hba: force scram-sha-256 for $DB/$ROLE from 127.0.0.1 (add at the top)
cp -f "$HBA" "$HBA.scrambak"
printf 'host    %s    %s    127.0.0.1/32    scram-sha-256\n' "$DB" "$ROLE" > "$HBA.new"
cat "$HBA" >> "$HBA.new"
mv -f "$HBA.new" "$HBA"
reload
sleep 1

# 3) SCRAM roundtrip: run the lk script (127.0.0.1 → the scram-sha-256 branch is mandatory)
echo '=== SCRAM roundtrip (lk) ==='
OUT=$("$LK" "$SCRIPT" 2>&1)
echo "lk output: $OUT"
if echo "$OUT" | grep -q '^4242$'; then
  echo 'PASS: SCRAM-SHA-256 login roundtrip SUCCEEDED (with the unified look:: crypto)'
  exit 0
else
  echo 'FAIL: SCRAM login roundtrip failed (auth rejected or wrong result)'
  exit 1
fi

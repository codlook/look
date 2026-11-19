#!/usr/bin/env bash
# PostgreSQL TLS — REAL SERVER interop (NOT a mock). On GitHub Actions ubuntu-latest, brings up the real postgres installed
# with apt as two clusters via initdb:
#   * TLS-ON  cluster (ssl=on, CA-signed cert, SAN=DNS:localhost, scram-sha-256 auth)
#   * TLS-OFF cluster (ssl=off) — for the silent-plaintext-fallback guard
# and measures LOOK's REAL postgresqls:// SSLRequest→'S'→TLS-upgrade→startup→SCRAM-auth→query
# roundtrip.
#
# The axis the MOCK (pg_tls_handshake_test.sh) COULD NOT PROVE, that THIS proves:
#   - The mock python TLS server only measures the client's verify DECISION (sends S, wraps TLS,
#     does not read the startup message, does no auth). "Does the PG protocol work after S" stays OPEN.
#   - This harness is a real postgres: the 'S' byte from a real server, a real Startup inside TLS + real
#     SCRAM-SHA-256 authentication + a real "SELECT" row return. So the protocol seam after the TLS upgrade
#     (wire over the encrypted channel) is proven END TO END.
#
# MATRIX (all against the real server):
#   1) valid     : SSL_CERT_FILE=ca + SAN matches → SELECT returns        (verify PASS + roundtrip)
#   2) untrusted : no CA, verify (default) → certificate verify fail → REJECT
#   3) mismatch  : SSL_CERT_FILE=ca BUT 127.0.0.1 (SAN=localhost) → hostname → REJECT
#   4) insecure  : ?tls=insecure, connects DESPITE an untrusted cert → SELECT (POSITIVE CONTROL:
#                  the rejection in 2/3 comes from verify, not from a broken handshake)
#   5) reject-tls: postgresqls:// → ssl=off server → 'N' → clean error (NO SILENT PLAINTEXT FALLBACK)
#   6) plain-ok  : postgres:// → ssl=off server → SELECT (POSITIVE CONTROL: the rejection in 5 is a TLS refusal,
#                  not a server-down)
set -u
LK="${1:-./build/lk}"
TLS_PORT="${2:-54330}"
PLAIN_PORT="${3:-54331}"
TMP=$(mktemp -d)
TLSDIR="$TMP/pg_tls"
PLAINDIR="$TMP/pg_plain"
CERTS="$TMP/certs"
PGT_PID=""; PGP_PID=""
fail=0

cleanup(){
  [ -n "$PGT_PID" ] && "$PGBIN/pg_ctl" -D "$TLSDIR"  -m immediate stop >/dev/null 2>&1
  [ -n "$PGP_PID" ] && "$PGBIN/pg_ctl" -D "$PLAINDIR" -m immediate stop >/dev/null 2>&1
  rm -rf "$TMP"
}
trap cleanup EXIT

command -v openssl >/dev/null 2>&1 || { echo "  (skipped: openssl missing)"; exit 0; }

# find the postgres binary directory (apt: /usr/lib/postgresql/<ver>/bin)
PGBIN=""
if command -v initdb >/dev/null 2>&1; then PGBIN="$(dirname "$(command -v initdb)")"; fi
if [ -z "$PGBIN" ]; then
  cand="$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -1)"
  [ -n "$cand" ] && PGBIN="$cand"
fi
[ -n "$PGBIN" ] && [ -x "$PGBIN/initdb" ] || { echo "  (skipped: postgres server binary missing)"; exit 0; }
echo "  PGBIN=$PGBIN"

# ── Certificates: our own CA + a CA-signed server cert (SAN=DNS:localhost) ──────
mkdir -p "$CERTS"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.crt" \
  -days 1 -subj "/CN=look-test-ca" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/server.key" -out "$CERTS/server.csr" \
  -subj "/CN=localhost" >/dev/null 2>&1
openssl x509 -req -in "$CERTS/server.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
  -CAcreateserial -days 1 -out "$CERTS/server.crt" \
  -extfile <(printf "subjectAltName=DNS:localhost\n") >/dev/null 2>&1
chmod 600 "$CERTS/server.key"
[ -s "$CERTS/server.crt" ] || { echo "  (skipped: could not generate certs)"; exit 0; }

# ── initdb: two clusters (scram default) ─────────────────────────────────────
"$PGBIN/initdb" -D "$TLSDIR"   -U postgres --auth=trust --auth-host=scram-sha-256 \
  --pwfile=<(echo postgres) >/dev/null 2>&1
"$PGBIN/initdb" -D "$PLAINDIR" -U postgres --auth=trust --auth-host=scram-sha-256 \
  --pwfile=<(echo postgres) >/dev/null 2>&1
[ -f "$TLSDIR/PG_VERSION" ] && [ -f "$PLAINDIR/PG_VERSION" ] || { echo "  (skipped: initdb failed)"; exit 0; }

# TLS-ON cluster config
cp "$CERTS/server.crt" "$TLSDIR/server.crt"; cp "$CERTS/server.key" "$TLSDIR/server.key"
chmod 600 "$TLSDIR/server.key"
cat >> "$TLSDIR/postgresql.conf" <<EOF
listen_addresses = 'localhost'
port = $TLS_PORT
unix_socket_directories = '$TLSDIR'
ssl = on
ssl_cert_file = 'server.crt'
ssl_key_file = 'server.key'
password_encryption = scram-sha-256
EOF
# scram + TLS required (hostssl); loopback IPv4/IPv6
cat > "$TLSDIR/pg_hba.conf" <<EOF
local   all all                trust
hostssl all all 127.0.0.1/32   scram-sha-256
hostssl all all ::1/128        scram-sha-256
EOF

# TLS-OFF cluster config
cat >> "$PLAINDIR/postgresql.conf" <<EOF
listen_addresses = 'localhost'
port = $PLAIN_PORT
unix_socket_directories = '$PLAINDIR'
ssl = off
password_encryption = scram-sha-256
EOF
cat > "$PLAINDIR/pg_hba.conf" <<EOF
local all all              trust
host  all all 127.0.0.1/32 scram-sha-256
host  all all ::1/128      scram-sha-256
EOF

"$PGBIN/pg_ctl" -D "$TLSDIR"   -l "$TMP/tls.log"   -w -t 30 start >/dev/null 2>&1 && PGT_PID=1
"$PGBIN/pg_ctl" -D "$PLAINDIR" -l "$TMP/plain.log" -w -t 30 start >/dev/null 2>&1 && PGP_PID=1
[ -n "$PGT_PID" ] && [ -n "$PGP_PID" ] || {
  echo "  (skipped: postgres could not be started)"; sed -n '1,20p' "$TMP/tls.log" "$TMP/plain.log" 2>/dev/null; exit 0; }

# scram-password app role + db (both clusters)
for D in "$TLSDIR" "$PLAINDIR"; do
  PT=$TLS_PORT; [ "$D" = "$PLAINDIR" ] && PT=$PLAIN_PORT
  PGPASSWORD=postgres "$PGBIN/psql" -h 127.0.0.1 -p "$PT" -U postgres -d postgres -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<SQL
SET password_encryption = 'scram-sha-256';
CREATE ROLE looktest LOGIN PASSWORD 'testpass';
CREATE DATABASE testdb OWNER looktest;
SQL
done

# ── .lk helpers (DSN from env; host/port under harness control) ───────────
cat > "$TMP/ok.lk" <<'LKEOF'
$db = db::connect(env("DSN"))
print(db::query($db, "SELECT 42 AS n")[0]["n"])
LKEOF
cat > "$TMP/reject.lk" <<'LKEOF'
try {
  $db = db::connect(env("DSN"))
  db::query($db, "SELECT 1")
  print("ACCEPTED")
} catch($e) { print("REJECTED") }
LKEOF

CA="$CERTS/ca.crt"
assert(){ # label expected actual
  if echo "$3" | grep -q "$2"; then echo "  OK  $1"; else echo "  FAIL $1 -> [$3]"; fail=1; fi
}

# 1) valid — verify PASS + real TLS roundtrip (SCRAM auth + SELECT inside TLS)
o=$(SSL_CERT_FILE="$CA" DSN="postgresqls://looktest:testpass@localhost:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "valid: trusted CA + SAN matches -> SELECT 42 (TLS roundtrip+SCRAM)" "42" "$o"

# 2) untrusted — no CA, verify default -> REJECT
o=$(DSN="postgresqls://looktest:testpass@localhost:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "untrusted: not in the CA store + verify -> REJECTED" "REJECTED" "$o"

# 3) mismatch — CA trusted but 127.0.0.1 (SAN=localhost) -> hostname REJECT
o=$(SSL_CERT_FILE="$CA" DSN="postgresqls://looktest:testpass@127.0.0.1:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "mismatch: valid CA but SAN!=127.0.0.1 -> hostname REJECTED" "REJECTED" "$o"

# 4) insecure — POSITIVE CONTROL: connects without verification
o=$(DSN="postgresqls://looktest:testpass@127.0.0.1:$TLS_PORT/testdb?tls=insecure" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "insecure: connects DESPITE an untrusted cert (verify off) -> 42" "42" "$o"

# 5) reject-tls — ssl=off server, postgresqls:// -> 'N' -> clean error (NO SILENT FALLBACK)
o=$(DSN="postgresqls://looktest:testpass@localhost:$PLAIN_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "reject-tls: ssl=off server + postgresqls:// -> REJECTED (NO plaintext downgrade)" "REJECTED" "$o"

# 6) plain-ok — POSITIVE CONTROL: postgres:// connects to the ssl=off server
o=$(DSN="postgres://looktest:testpass@localhost:$PLAIN_PORT/testdb" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "plain-ok: postgres:// ssl=off server -> 42 (case 5's rejection is a TLS refusal, not server-down)" "42" "$o"

echo
[ $fail = 0 ] && echo "PASS: PG TLS real-server interop (SSLRequest->TLS->startup->SCRAM->query)" \
              || echo "FAIL: PG TLS real-server interop"
exit $fail

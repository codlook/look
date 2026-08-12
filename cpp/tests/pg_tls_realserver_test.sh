#!/usr/bin/env bash
# PostgreSQL TLS — GERÇEK SUNUCU interop (mock DEĞİL). GitHub Actions ubuntu-latest'te
# apt ile kurulan gerçek postgres'i initdb ile iki cluster olarak ayağa kaldırır:
#   * TLS-ON  cluster (ssl=on, CA-imzalı cert, SAN=DNS:localhost, scram-sha-256 auth)
#   * TLS-OFF cluster (ssl=off) — sessiz-plaintext-düşüş guard'ı için
# ve LOOK'un postgresqls:// ile GERÇEK SSLRequest→'S'→TLS-upgrade→startup→SCRAM-auth→query
# roundtrip'ini ölçer.
#
# MOCK'un (pg_tls_handshake_test.sh) KANITLAYAMADIĞI, BUNUN kanıtladığı eksen:
#   - Mock python TLS sunucusu yalnız client'ın verify KARARINI ölçer (S gönderir, TLS sarar,
#     startup mesajını okumaz, auth yapmaz). "S sonrası PG protokolü çalışıyor mu" AÇIKTA kalır.
#   - Bu harness gerçek postgres: 'S' baytı gerçek sunucudan, TLS içinde gerçek Startup + gerçek
#     SCRAM-SHA-256 authentication + gerçek "SELECT" satır dönüşü. Yani TLS-upgrade sonrası
#     protokol dikişi (encrypted kanal üstünde wire) UÇTAN UCA kanıtlanır.
#
# MATRİS (hepsi gerçek sunucuya karşı):
#   1) valid     : SSL_CERT_FILE=ca + SAN eşleşir → SELECT döner        (verify PASS + roundtrip)
#   2) untrusted : CA yok, verify(varsayılan) → certificate verify fail → RED
#   3) mismatch  : SSL_CERT_FILE=ca AMA 127.0.0.1 (SAN=localhost) → hostname → RED
#   4) insecure  : ?tls=insecure, güvenilmez cert'e RAĞMEN bağlanır → SELECT (POZİTİF KONTROL:
#                  2/3 reddi verify'dan gelir, bozuk handshake'ten değil)
#   5) reject-tls: postgresqls:// → ssl=off sunucu → 'N' → temiz hata (SESSİZ PLAINTEXT DÜŞÜŞ YOK)
#   6) plain-ok  : postgres:// → ssl=off sunucu → SELECT (POZİTİF KONTROL: 5'in reddi TLS-refuse,
#                  sunucu-down değil)
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

command -v openssl >/dev/null 2>&1 || { echo "  (atlandi: openssl yok)"; exit 0; }

# postgres binary dizinini bul (apt: /usr/lib/postgresql/<ver>/bin)
PGBIN=""
if command -v initdb >/dev/null 2>&1; then PGBIN="$(dirname "$(command -v initdb)")"; fi
if [ -z "$PGBIN" ]; then
  cand="$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -1)"
  [ -n "$cand" ] && PGBIN="$cand"
fi
[ -n "$PGBIN" ] && [ -x "$PGBIN/initdb" ] || { echo "  (atlandi: postgres server binary yok)"; exit 0; }
echo "  PGBIN=$PGBIN"

# ── Sertifikalar: kendi CA'mız + CA-imzalı server cert (SAN=DNS:localhost) ──────
mkdir -p "$CERTS"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.crt" \
  -days 1 -subj "/CN=look-test-ca" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/server.key" -out "$CERTS/server.csr" \
  -subj "/CN=localhost" >/dev/null 2>&1
openssl x509 -req -in "$CERTS/server.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
  -CAcreateserial -days 1 -out "$CERTS/server.crt" \
  -extfile <(printf "subjectAltName=DNS:localhost\n") >/dev/null 2>&1
chmod 600 "$CERTS/server.key"
[ -s "$CERTS/server.crt" ] || { echo "  (atlandi: cert uretilemedi)"; exit 0; }

# ── initdb: iki cluster (scram varsayilan) ─────────────────────────────────────
"$PGBIN/initdb" -D "$TLSDIR"   -U postgres --auth=trust --auth-host=scram-sha-256 \
  --pwfile=<(echo postgres) >/dev/null 2>&1
"$PGBIN/initdb" -D "$PLAINDIR" -U postgres --auth=trust --auth-host=scram-sha-256 \
  --pwfile=<(echo postgres) >/dev/null 2>&1
[ -f "$TLSDIR/PG_VERSION" ] && [ -f "$PLAINDIR/PG_VERSION" ] || { echo "  (atlandi: initdb basarisiz)"; exit 0; }

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
# scram + TLS zorunlu (hostssl); loopback IPv4/IPv6
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
  echo "  (atlandi: postgres baslatilamadi)"; sed -n '1,20p' "$TMP/tls.log" "$TMP/plain.log" 2>/dev/null; exit 0; }

# scram parolali app rolu + db (her iki cluster)
for D in "$TLSDIR" "$PLAINDIR"; do
  PT=$TLS_PORT; [ "$D" = "$PLAINDIR" ] && PT=$PLAIN_PORT
  PGPASSWORD=postgres "$PGBIN/psql" -h 127.0.0.1 -p "$PT" -U postgres -d postgres -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<SQL
SET password_encryption = 'scram-sha-256';
CREATE ROLE looktest LOGIN PASSWORD 'testpass';
CREATE DATABASE testdb OWNER looktest;
SQL
done

# ── .lk yardimcilari (DSN env'den; host/port harness kontrolunde) ──────────────
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

# 1) valid — verify PASS + gercek TLS roundtrip (SCRAM auth + SELECT icinde TLS)
o=$(SSL_CERT_FILE="$CA" DSN="postgresqls://looktest:testpass@localhost:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "valid: CA-guvenilir + SAN eslesir -> SELECT 42 (TLS roundtrip+SCRAM)" "42" "$o"

# 2) untrusted — CA yok, verify default -> RED
o=$(DSN="postgresqls://looktest:testpass@localhost:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "untrusted: CA store'da yok + verify -> REJECTED" "REJECTED" "$o"

# 3) mismatch — CA guvenilir ama 127.0.0.1 (SAN=localhost) -> hostname RED
o=$(SSL_CERT_FILE="$CA" DSN="postgresqls://looktest:testpass@127.0.0.1:$TLS_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "mismatch: gecerli CA ama SAN!=127.0.0.1 -> hostname REJECTED" "REJECTED" "$o"

# 4) insecure — POZITIF KONTROL: dogrulamasiz baglanir
o=$(DSN="postgresqls://looktest:testpass@127.0.0.1:$TLS_PORT/testdb?tls=insecure" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "insecure: guvenilmez cert'e RAGMEN baglanir (verify off) -> 42" "42" "$o"

# 5) reject-tls — ssl=off sunucu, postgresqls:// -> 'N' -> temiz hata (SESSIZ DUSUS YOK)
o=$(DSN="postgresqls://looktest:testpass@localhost:$PLAIN_PORT/testdb" \
      timeout 20 "$LK" "$TMP/reject.lk" 2>&1)
assert "reject-tls: ssl=off sunucu + postgresqls:// -> REJECTED (plaintext dususu YOK)" "REJECTED" "$o"

# 6) plain-ok — POZITIF KONTROL: postgres:// ssl=off sunucuya baglanir
o=$(DSN="postgres://looktest:testpass@localhost:$PLAIN_PORT/testdb" \
      timeout 20 "$LK" "$TMP/ok.lk" 2>&1)
assert "plain-ok: postgres:// ssl=off sunucu -> 42 (5'in reddi TLS-refuse, down degil)" "42" "$o"

echo
[ $fail = 0 ] && echo "PASS: PG TLS gercek-sunucu interop (SSLRequest->TLS->startup->SCRAM->query)" \
              || echo "FAIL: PG TLS gercek-sunucu interop"
exit $fail

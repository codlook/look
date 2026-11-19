#!/usr/bin/env bash
# PostgreSQL TLS verify — real handshake, NO Docker/live PG. Measures the verify decision of the LOOK client against a mock TLS PG server
# (SSLRequest→'S'→TLS, self-signed cert).
#
# Main point: verify-full (postgresqls:// default) must REJECT an UNTRUSTED cert (is SSL_VERIFY_PEER
# working). POSITIVE CONTROL: when connecting to the same server insecure (?sslmode=require) the TLS
# handshake COMPLETES (mock log: verify→HANDSHAKE_FAIL client-abort, insecure→HANDSHAKE_OK).
# If verify were broken an untrusted cert would be accepted + the handshake would be OK.
set -u
LK="${1:-./build/lk}"
PORT="${2:-7799}"
TMP=$(mktemp -d)
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 0.5; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
command -v python3 >/dev/null 2>&1 || { echo "  (atlandi: python3)"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  (atlandi: openssl)"; exit 0; }
fail=0

# Self-signed server cert (SAN=127.0.0.1) — UNTRUSTED (not in the client trust store)
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/k.pem" -out "$TMP/c.pem" \
  -days 1 -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1

cat > "$TMP/mock.py" <<'PYEOF'
import socket, ssl, sys, threading
port=int(sys.argv[1]); cert=sys.argv[2]; key=sys.argv[3]; logf=sys.argv[4]
def log(m):
    with open(logf,"a") as f: f.write(m+"\n")
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert,key)
srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
srv.bind(("127.0.0.1",port)); srv.listen(5); srv.settimeout(20)
def handle(c):
    try:
        c.recv(8)                 # SSLRequest (8 bytes)
        c.sendall(b'S')           # SSL allowed
        try:
            tls=ctx.wrap_socket(c,server_side=True)   # TLS handshake
            log("HANDSHAKE_OK")                        # the client ACCEPTED the cert
            try: tls.recv(64)
            except Exception: pass
            tls.close()
        except ssl.SSLError:
            log("HANDSHAKE_FAIL")                      # the client REJECTED the cert (abort)
        except Exception:
            log("HANDSHAKE_FAIL")
    except Exception:
        pass
    try: c.close()
    except Exception: pass
try:
    while True:
        try: c,_=srv.accept()
        except socket.timeout: break
        threading.Thread(target=handle,args=(c,),daemon=True).start()
except Exception: pass
PYEOF

: > "$TMP/mock.log"
python3 "$TMP/mock.py" "$PORT" "$TMP/c.pem" "$TMP/k.pem" "$TMP/mock.log" &
SRV=$!
sleep 1

# 1) verify-full (postgresqls:// default, no SSL_CERT_FILE) → untrusted → must be REJECTED
cat > "$TMP/v.lk" <<LKEOF
try {
  \$db = db::connect("postgresqls://u:p@127.0.0.1:$PORT/testdb")
  db::query(\$db, "SELECT 1")
  print("VERIFY_REJECTED=false")
} catch(\$e) { print("VERIFY_REJECTED=true") }
LKEOF
vout=$(timeout 15 "$LK" "$TMP/v.lk" 2>&1)
# 2) insecure (?sslmode=require) → TLS must complete (the cert is not verified)
cat > "$TMP/i.lk" <<LKEOF
try {
  \$db = db::connect("postgres://u:p@127.0.0.1:$PORT/testdb?sslmode=require")
  db::query(\$db, "SELECT 1")
  print("done")
} catch(\$e) { print("done") }
LKEOF
timeout 15 "$LK" "$TMP/i.lk" >/dev/null 2>&1
sleep 1

hs_ok=$(grep -c 'HANDSHAKE_OK'   "$TMP/mock.log")
hs_fail=$(grep -c 'HANDSHAKE_FAIL' "$TMP/mock.log")

# Assert: verify → LOOK rejected AND the mock handshake FAIL (the client aborted on the cert)
if echo "$vout" | grep -q 'VERIFY_REJECTED=true'; then echo "  OK verify-full untrusted cert'i REDDETTI (LOOK throw)"
else echo "  FAIL verify-full untrusted cert'i KABUL etti (verify kırık): [$vout]"; fail=1; fi
if [ "$hs_fail" -ge 1 ]; then echo "  OK mock: verify-case handshake FAIL (client cert-abort)"
else echo "  FAIL mock: handshake FAIL görülmedi (LOOK cert'i reddetmedi?)"; fail=1; fi
# Positive control: insecure → the handshake COMPLETED (verify skipped, TLS established)
if [ "$hs_ok" -ge 1 ]; then echo "  OK pozitif-kontrol: insecure handshake OK (TLS kuruldu, cert doğrulanmadı)"
else echo "  FAIL pozitif-kontrol: insecure handshake OK görülmedi (TLS hiç kurulmadı? test sağlıksız)"; fail=1; fi

[ $fail = 0 ] && echo "PASS: PG TLS verify (untrusted-reject + insecure-accept)" || echo "FAIL: PG TLS verify"
exit $fail

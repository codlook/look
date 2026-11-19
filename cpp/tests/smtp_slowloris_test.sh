#!/bin/bash
# SMTP slowloris / idle-timeout guard — protection at the EventLoop layer.
# The protection that exists in IMAP with SO_RCVTIMEO was
# MISSING in SMTP's async EventLoop path. Since SO_RCVTIMEO does not work on a non-blocking ET socket the protection
# was added to the EventLoop (epoll_wait with a finite tick + idle scan).
#
# POSITIVE CONTROL (README rule 2): measure that the guard REALLY fires —
#  1) an idle connection (one that gets the banner and sends no data at all) must close AFTER the timeout,
#     NOT before (i.e. what triggers the close is really the idle timeout).
#  2) an ACTIVE connection (sending NOOP) must stay open EVEN if it exceeds the timeout (no false positive).
#  3) after the idle reap conn_count must drop → a new connection is accepted (no fd/counter leak).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7830}"
HTTP_PORT=$((PORT + 100))
IDLE=3
TMP=$(mktemp -d)
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail=0
command -v python3 >/dev/null 2>&1 || { echo "  (atlandi: python3 gerekli)"; exit 0; }

echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
mkdir -p "$TMP/md"
LOOK_SMTP_PORT=$PORT LOOK_MAIL_DIR="$TMP/md" LOOK_SMTP_LOCAL_DOMAINS=localhost \
  LOOK_SMTP_IDLE_TIMEOUT=$IDLE LOOK_SMTP_MAX_CONNS_IP=100 \
  "$FCGI" --mode http --port $HTTP_PORT "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null && break; sleep 0.5; done
sleep 1

cat > "$TMP/slow.py" <<PYEOF
import socket, time, os, sys
HOST="127.0.0.1"; PORT=$PORT; IDLE=$IDLE; PID="$SRV"
def fdcount():
    try: return len(os.listdir("/proc/%s/fd" % PID))
    except Exception: return -1

# 1) IDLE connection: get the banner, send no data at all → measure the time to close
s=socket.create_connection((HOST,PORT),timeout=IDLE+10); s.settimeout(IDLE+10)
s.recv(256)  # banner
t0=time.time()
closed_at=None
try:
    while time.time()-t0 < IDLE+8:
        d=s.recv(256)      # EOF (b"") or RST → closed
        if d==b"": closed_at=time.time()-t0; break
except Exception:
    closed_at=time.time()-t0
s.close()

# 2) ACTIVE connection: exceed the timeout by sending NOOP → must stay open
a=socket.create_connection((HOST,PORT),timeout=IDLE+10); a.settimeout(IDLE+10)
a.recv(256)
active_alive=True
try:
    for _ in range(int((IDLE*2)/1)+1):
        a.sendall(b"NOOP\r\n")
        r=a.recv(256)
        if r==b"": active_alive=False; break
        time.sleep(1)
except Exception:
    active_alive=False
a.close()

# 3) a new connection is accepted after the reap (conn_count did not leak)
time.sleep(1)
try:
    n=socket.create_connection((HOST,PORT),timeout=5); n.settimeout(5)
    new_ok = n.recv(256).startswith(b"220"); n.close()
except Exception:
    new_ok=False

ca = -1 if closed_at is None else round(closed_at,2)
print("CLOSED_AT=%s ACTIVE_ALIVE=%s NEW_OK=%s" % (ca, active_alive, new_ok))
PYEOF

out=$(timeout 60 python3 "$TMP/slow.py" 2>&1 | tail -1)
echo "  $out"
ca=$(echo "$out" | sed -n 's/.*CLOSED_AT=\([0-9.-]*\).*/\1/p')
aa=$(echo "$out" | grep -o 'ACTIVE_ALIVE=True' || true)
no=$(echo "$out" | grep -o 'NEW_OK=True' || true)

# the idle close must be in the IDLE..IDLE+5 range (NOT before — positive control)
if awk "BEGIN{exit !($ca >= $IDLE-0.5 && $ca <= $IDLE+5)}" 2>/dev/null; then
  echo "  PASS idle bağlantı ~${ca}s sonra kapandı (timeout=${IDLE}s) — slowloris reap ATEŞLEDİ"
else
  echo "  FAIL idle kapanma zamanı beklenmedik: ${ca}s (timeout ${IDLE}s)"; fail=1
fi
[ -n "$aa" ] && echo "  PASS aktif (NOOP) bağlantı timeout'u aşarak açık kaldı — yanlış-pozitif yok" \
             || { echo "  FAIL aktif bağlantı yanlışlıkla kapandı (yanlış-pozitif reap)"; fail=1; }
[ -n "$no" ] && echo "  PASS reap sonrası yeni bağlantı kabul (conn_count/fd sızmadı)" \
             || { echo "  FAIL reap sonrası yeni bağlantı reddedildi (sayaç sızıntısı?)"; fail=1; }

[ $fail = 0 ] && echo "PASS: SMTP slowloris idle-timeout" || echo "FAIL: SMTP slowloris idle-timeout"
exit $fail

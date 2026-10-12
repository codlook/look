#!/bin/bash
# SMTP gec yanit guard'i — teslim yaniti YANLIS baglantiya yazilmamali.
#
# HATA (olculdu, 4/4): worker mesaji bitirince "250 2.6.0 Message accepted"
# yanitini DATA bittigi andaki ham fd NUMARASINA yaziyordu. Istemci worker
# mesgulken koptuysa o fd kapanmis, araya giren yeni baglanti ayni numarayi
# almis oluyordu → yanit ILGISIZ baglantiya, 220 banner'inin hemen ardindan
# gidiyordu; o istemcinin komut/yanit eslesmesi bir kayiyordu.
#
# WORKER'I BELIRLENIMLI TUTMA: gercek hatada worker'i yavas DNS (SPF) tutuyordu;
# guard DNS'e BAGLI OLAMAZ. Bunun yerine STARTTLS kullanilir: el sikisma ayni
# worker havuzunda, bloklayan SSL_accept ile yapilir ve teslimden ONCELIKLIDIR.
# W tane istemci STARTTLS deyip ClientHello GONDERMEZSE W worker'in hepsi
# bekler; teslim kuyrukta kalir. Tutucular kapaninca worker'lar serbest kalir.
# MAIL FROM:<> (null sender) kullanilir → SPF hic DNS sormaz.
#
# POZITIF KONTROLLER (guard sessizce gecmesin):
#  0) bekletme YOKKEN teslim 250'yi kendi baglantisina alir (duzeltme mesru
#     yaniti dusurmuyor),
#  1) tutucular varken mesaj Maildir'e DUSMEZ (worker gercekten tutuluyor),
#  2) tutucular kapaninca mesaj DUSER (yanit ani gercekten yasandi),
#  3) yeni baglanti sonradan NOOP'a tam olarak kendi yanitini alir.
#
# Kullanim: bash smtp_late_reply_test.sh <lk-fcgi_yolu> [port]
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7834}"
HTTP_PORT=$((PORT + 100))
W=2
TMP=$(mktemp -d)
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  (skipped: openssl is required)"; exit 0; }

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" \
  -keyout "$TMP/key.pem" -out "$TMP/cert.pem" >/dev/null 2>&1 \
  || { echo "FAIL: could not create a test certificate"; exit 1; }

echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
mkdir -p "$TMP/maildir"
LOOK_SMTP_PORT=$PORT LOOK_MAIL_DIR="$TMP/maildir" LOOK_SMTP_LOCAL_DOMAINS=localhost \
  LOOK_SMTP_CERT="$TMP/cert.pem" LOOK_SMTP_KEY="$TMP/key.pem" \
  "$FCGI" --mode http --port $HTTP_PORT --workers $W "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null && break; sleep 0.5; done
sleep 1

cat > "$TMP/late.py" <<'PYEOF'
import glob, socket, sys, time
HOST = "127.0.0.1"; PORT = int(sys.argv[1]); W = int(sys.argv[2]); MD = sys.argv[3]

def conn():
    s = socket.create_connection((HOST, PORT), timeout=5); s.settimeout(5); return s
def rd(s, t=5):
    s.settimeout(t)
    try: return s.recv(4096).decode("utf-8", "replace")
    except Exception: return ""
def cmd(s, x): s.sendall((x + "\r\n").encode()); return rd(s)
def teslim(): return len(glob.glob(MD + "/*/inbox/new/*"))
def bitir(m): print(m); sys.exit(0)

def zarf(s):
    if not rd(s).startswith("220"): bitir("FAIL: no 220 banner")
    cmd(s, "EHLO t"); cmd(s, "MAIL FROM:<>"); cmd(s, "RCPT TO:<u@localhost>")
    if not cmd(s, "DATA").startswith("354"): bitir("FAIL: DATA was not accepted")
    s.sendall(b"Subject: G\r\n\r\ngovde\r\n.\r\n")

# 0) Bekletme yok: yanit kendi baglantisina gelmeli.
k = conn(); zarf(k)
y = rd(k, 15)
if not y.startswith("250 2.6.0"): bitir("FAIL: control delivery got [%s], expected 250 2.6.0" % y.strip())
k.close()
taban = teslim()
if taban != 1: bitir("FAIL: control delivery wrote %d files, expected 1" % taban)

# 1) W worker'i STARTTLS el sikismasinda tut (ClientHello gonderilmez).
tutucu = []
for _ in range(W):
    s = conn(); rd(s); cmd(s, "EHLO t")
    if not cmd(s, "STARTTLS").startswith("220"): bitir("FAIL: STARTTLS was not offered")
    tutucu.append(s)
time.sleep(0.7)

# 2) A: mesaji bitir, yaniti BEKLEMEDEN kop.
a = conn(); zarf(a)
time.sleep(1.0)
if teslim() != taban: bitir("FAIL: the worker was not held (message delivered before the release)")
a.close()
time.sleep(1.0)

# 3) B: A'nin biraktigi fd numarasini alir. Hicbir sey gondermez.
b = [conn() for _ in range(3)]
for s in b:
    if not rd(s).startswith("220"): bitir("FAIL: new connection got no 220 banner")
if teslim() != taban: bitir("FAIL: the worker was not held (message delivered before the release)")

# 4) Tutuculari birak → worker A'nin mesajini teslim eder ve yanitini yollar.
for s in tutucu: s.close()
for _ in range(60):
    if teslim() > taban: break
    time.sleep(0.25)
else:
    bitir("FAIL: the held message was never delivered")

# 5) B'ye hicbir sey gelmemeli; sonra NOOP kendi yanitini almali.
time.sleep(1.5)
for i, s in enumerate(b):
    s.settimeout(0.3)
    try: d = s.recv(4096)
    except socket.timeout: d = None
    except Exception as e: bitir("FAIL: new connection %d broke: %s" % (i, type(e).__name__))
    if d: bitir("FAIL: new connection %d received another session's reply: [%s]" % (i, d.decode("utf-8", "replace").strip()))
    if d == b"": bitir("FAIL: new connection %d was closed by the server" % i)
for i, s in enumerate(b):
    y = cmd(s, "NOOP").strip()
    if y != "250 2.0.0 OK": bitir("FAIL: new connection %d got [%s] for NOOP" % (i, y))
bitir("OK")
PYEOF

out=$(timeout 90 python3 "$TMP/late.py" $PORT $W "$TMP/maildir" 2>&1 | tail -1)
if [ "$out" = "OK" ]; then
  echo "  PASS a late delivery reply is not written to a connection that reused the fd"
  echo "PASS: SMTP late reply"
  exit 0
fi
echo "  ${out:-FAIL: no result from the probe}"
echo "FAIL: SMTP late reply"
exit 1

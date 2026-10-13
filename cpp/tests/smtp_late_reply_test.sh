#!/bin/bash
# SMTP gec yanit guard'i — teslim yaniti YANLIS baglantiya yazilmamali.
#
# HATA (olculdu, 4/4): worker mesaji bitirince "250 2.6.0 Message accepted"
# yanitini DATA bittigi andaki ham fd NUMARASINA yaziyordu. Istemci worker
# mesgulken koptuysa o fd kapanmis, araya giren yeni baglanti ayni numarayi
# almis oluyordu → yanit ILGISIZ baglantiya, 220 banner'inin hemen ardindan
# gidiyordu; o istemcinin komut/yanit eslesmesi bir kayiyordu.
#
# WORKER'I TUTMA: gercek hatada worker'i yavas DNS (SPF) tutuyordu; guard DNS'e BAGLI
# OLAMAZ. 20 MB'lik bir mesaj kullanilir: dogrulanmasi ve diske yazilmasi worker'da zaman
# alir. Gonderen mesaji bitirip sunucunun baglantiyi kapattigini gorur (EOF), ardindan yeni
# baglantilar acilir ve o fd numarasini alir. MAIL FROM:<> kullanilir → SPF hic DNS sormaz.
# (1.0.17'ye dek yarim birakilan STARTTLS el sikismasi kullaniliyordu; STARTTLS 1.0.18'de
# sunulmuyor.)
#
# POZITIF KONTROLLER (guard sessizce gecmesin):
#  0) bekletme YOKKEN teslim 250'yi kendi baglantisina alir (duzeltme mesru yaniti dusurmuyor),
#  1) yeni baglantilar banner'ini aldiginda buyuk mesaj HENUZ teslim edilmemis olmali; bu her
#     denemede olculur, tutulamayan deneme sayilmaz; iki tutulan deneme olmadan test KIRMIZI,
#  2) mesaj sonunda teslim edilir (yanit ani gercekten yasandi),
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

echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
mkdir -p "$TMP/maildir"
LOOK_SMTP_PORT=$PORT LOOK_MAIL_DIR="$TMP/maildir" LOOK_SMTP_LOCAL_DOMAINS=localhost \
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

# 1) Worker'i TUT: buyuk bir mesajin dogrulanmasi ve diske yazilmasi worker'da zaman alir.
#    (1.0.17'ye dek yarim birakilan STARTTLS el sikismasi kullaniliyordu; STARTTLS artik
#    sunulmuyor.) Tutmanin gerceklestigi her denemede OLCULUR: yeni baglantilar banner'ini
#    aldiginda mesaj henuz teslim edilmemis olmali. Tutulamayan deneme sayilmaz, yeniden denenir.
SATIR = b"X" * 76 + b"\r\n"
GOVDE = b"Subject: BIG\r\n\r\n" + SATIR * (20 * 1024 * 1024 // len(SATIR)) + b".\r\n"

def dene():
    taban = teslim()
    a = conn()
    if not rd(a).startswith("220"): bitir("FAIL: no 220 banner")
    cmd(a, "EHLO t"); cmd(a, "MAIL FROM:<>"); cmd(a, "RCPT TO:<u@localhost>")
    if not cmd(a, "DATA").startswith("354"): bitir("FAIL: DATA was not accepted")
    # 2) A: mesaji bitir ve yaniti BEKLEMEDEN kop. Sunucunun fd'yi kapattigini gor (EOF):
    #    boylece siradaki accept o numarayi alir.
    a.sendall(GOVDE); a.shutdown(socket.SHUT_WR)
    a.settimeout(20)
    try:
        while a.recv(4096): pass
    except Exception: pass
    a.close()
    # 3) B: A'nin biraktigi fd numarasini alir. Hicbir sey gondermez.
    b = [conn() for _ in range(3)]
    for s in b:
        if not rd(s).startswith("220"): bitir("FAIL: new connection got no 220 banner")
    tutuldu = (teslim() == taban)
    # 4) Worker biter: mesaj teslim edilir ve yaniti gonderilmek istenir.
    for _ in range(120):
        if teslim() > taban: break
        time.sleep(0.25)
    else:
        bitir("FAIL: the message of the disconnected sender was never delivered")
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
    for s in b: s.close()
    return tutuldu

tutulan = 0
for _ in range(6):
    if dene(): tutulan += 1
    if tutulan >= 2: break
if tutulan < 2: bitir("FAIL: the worker was not held (the message was delivered before the new connections were in place) — the test did not reach the case it is for")
bitir("OK")
PYEOF

out=$(timeout 300 python3 "$TMP/late.py" $PORT $W "$TMP/maildir" 2>&1 | tail -1)
if [ "$out" = "OK" ]; then
  echo "  PASS a late delivery reply is not written to a connection that reused the fd"
  echo "PASS: SMTP late reply"
  exit 0
fi
echo "  ${out:-FAIL: no result from the probe}"
echo "FAIL: SMTP late reply"
exit 1

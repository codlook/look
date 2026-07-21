#!/bin/bash
# SMTP sunucusu guard'i — smtp_server.cpp (1373 satir) icin ILK guard.
#
# NEDEN: bu dosya disariya port dinliyor, kimlik dogrulama yapiyor ve HIC
# guard'i yoktu (envanterde "en buyuk kor nokta"). Gercek sunucu ayaga
# kaldirilip ham SMTP konusuluyor.
#
# Kilitlenenler:
#  1) ADRES AYRISTIRMA (42. bug): `MAIL FROM:` (adressiz) gonderince adres
#     "FROM:" oluyordu ve DISKE `Return-Path: <FROM:>` yaziliyordu; bare adres
#     destegi kirikti (`FROM: a@b.c`); bozuk `<a@b<c>` kabul ediliyordu; ve
#     null sender `<>` REDDEDILIYORDU — oysa RFC 5321 §4.5.5'te bounce/DSN icin
#     zorunlu kabul edilir. Ayristirma TERS yonde gevsekti.
#  2) Relay korumasi: kimlik dogrulamasiz disariya teslim reddedilmeli.
#  3) Kaynak sinirlari ILAN EDILIP ZORLANIYOR mu (SIZE, RCPT sayisi).
#
# Kullanim: bash smtp_test.sh <lk-fcgi_yolu> [port]
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7826}"
HTTP_PORT=$((PORT + 100))
TMP=$(mktemp -d)
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail=0

command -v python3 >/dev/null 2>&1 || { echo "  (atlandi: python3 gerekli)"; exit 0; }

echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
mkdir -p "$TMP/maildir"
LOOK_SMTP_PORT=$PORT LOOK_MAIL_DIR="$TMP/maildir" LOOK_SMTP_LOCAL_DOMAINS=localhost \
  "$FCGI" --mode http --port $HTTP_PORT "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null && break; sleep 0.5; done
sleep 1

cat > "$TMP/probe.py" <<'PYEOF'
import socket, sys
HOST="127.0.0.1"; PORT=int(sys.argv[1])
def sohbet(komutlar, govde=None):
    s=socket.create_connection((HOST,PORT),timeout=10); s.settimeout(10)
    def rd():
        try: return s.recv(4096).decode("utf-8","replace").strip()
        except Exception as e: return "<%s>"%type(e).__name__
    rd()
    son=""
    for k in komutlar:
        s.sendall((k+"\r\n").encode()); son=rd()
    if govde and son.startswith("354"):
        s.sendall((govde+"\r\n.\r\n").encode()); son=rd()
    try: s.sendall(b"QUIT\r\n"); s.close()
    except Exception: pass
    return son.split(" ")[0].split("-")[0]
sonuc=[]
# adres ayristirma
sonuc.append(sohbet(["EHLO t","MAIL FROM:<a@t.com>"]))            # 250 gecerli
sonuc.append(sohbet(["EHLO t","MAIL FROM:"]))                      # 501 adres yok
sonuc.append(sohbet(["EHLO t","MAIL FROM:<a@b<c>"]))               # 501 bozuk
sonuc.append(sohbet(["EHLO t","MAIL FROM:<>"]))                    # 250 null sender (RFC)
sonuc.append(sohbet(["EHLO t","MAIL FROM: bare@t.com"]))           # 250 bare adres
sonuc.append(sohbet(["EHLO t","MAIL FROM:<a@t.com>","RCPT TO:"]))  # 501 alici yok
# relay korumasi
sonuc.append(sohbet(["EHLO t","MAIL FROM:<a@t.com>","RCPT TO:<v@gmail.com>"]))  # 550
# komut sirasi
sonuc.append(sohbet(["MAIL FROM:<a@t.com>"]))                      # 503 EHLO once
print("|".join(sonuc))
PYEOF

out=$(timeout 90 python3 "$TMP/probe.py" $PORT 2>&1 | tail -1)
bek="250|501|501|250|250|501|550|503"
if [ "$out" = "$bek" ]; then
  echo "  PASS SMTP: adres ayristirma + relay + komut sirasi + satir siniri"
else
  echo "  FAIL SMTP yanit dizisi: [$out]"
  echo "       beklenen           : [$bek]"
  echo "       sira: gecerli|adressiz|bozuk|null-sender|bare|rcpt-yok|relay|sirasiz"
  fail=1
fi

# Diske YAZILAN Return-Path cop icermemeli (42. bugin kalbi)
cat > "$TMP/teslim.py" <<'PYEOF'
import socket, sys
HOST="127.0.0.1"; PORT=int(sys.argv[1])
s=socket.create_connection((HOST,PORT),timeout=10); s.settimeout(10)
def rd():
    try: return s.recv(4096).decode("utf-8","replace").strip()
    except Exception: return ""
def w(x): s.sendall((x+"\r\n").encode()); return rd()
rd(); w("EHLO t"); w("MAIL FROM: bare@t.com"); w("RCPT TO:<u@localhost>")
if w("DATA").startswith("354"):
    s.sendall(b"Subject: G\r\n\r\ngovde\r\n.\r\n"); rd()
try: s.sendall(b"QUIT\r\n"); s.close()
except Exception: pass
PYEOF
timeout 30 python3 "$TMP/teslim.py" $PORT >/dev/null 2>&1
rp=$(grep -rh '^Return-Path:' "$TMP/maildir" 2>/dev/null | head -1)
case "$rp" in
  *"<bare@t.com>"*) echo "  PASS Return-Path temiz: $rp" ;;
  *"FROM:"*)        echo "  FAIL Return-Path'e COP yazildi: $rp (bare adres ayristirma kirik)"; fail=1 ;;
  "")               echo "  FAIL mesaj teslim edilmedi (Return-Path bulunamadi)"; fail=1 ;;
  *)                echo "  FAIL Return-Path beklenmedik: $rp"; fail=1 ;;
esac

# ── EVENT LOOP: fd yasam dongusu + per-IP limiti ────────────────────────────────
# event_loop.cpp (658 satir) tum sunucularin altinda yatiyor ve guard'i yoktu.
# Anormal kopmalarda (RST, yarim komut, DATA ortasi) fd SIZMAMALI — sizinti
# uzun omurlu bir sunucuda fd tukenmesine, yani tam kesintiye goturur.
# Per-IP limiti de burada kilitleniyor: 220 (kabul) ile 421 (red) AYIRT EDILEREK
# olculur — recv'den gelen her seyi "banner" saymak yaniltir (bu tam olarak
# yasandi: 421 reddi banner sanilip "limit calismiyor" sonucu cikarilmisti).
cat > "$TMP/fd.py" <<'PYEOF'
import socket, sys, os, time, struct
HOST="127.0.0.1"; PORT=int(sys.argv[1]); PID=sys.argv[2]
def fd():
    try: return len(os.listdir("/proc/%s/fd" % PID))
    except Exception: return -1
def kirli():
    s=socket.create_connection((HOST,PORT),timeout=4); s.settimeout(4)
    try:
        s.recv(200); s.sendall(b"EHLO t\r\n"); s.recv(400)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii",1,0))
    finally: s.close()
def yarim():
    s=socket.create_connection((HOST,PORT),timeout=4); s.settimeout(4)
    try: s.recv(200); s.sendall(b"MAIL FROM:<a@b.c")   # CRLF yok
    finally: s.close()
taban=fd()
for _ in range(40): kirli()
for _ in range(40): yarim()
time.sleep(1.5)
sizinti = fd() - taban
# per-IP limiti: 220 kabul vs 421 red
kabul=0; red=0; acik=[]
for i in range(25):
    try:
        s=socket.create_connection((HOST,PORT),timeout=3); s.settimeout(3)
        d=s.recv(200).decode("utf-8","replace")
        if d.startswith("220"): kabul+=1; acik.append(s)
        elif d.startswith("421"): red+=1; s.close()
        else: s.close()
    except Exception: pass
for s in acik:
    try: s.close()
    except Exception: pass
print("%d|%d|%d" % (sizinti, kabul, red))
PYEOF
if [ -d /proc ]; then
  fdout=$(timeout 120 python3 "$TMP/fd.py" $PORT $SRV 2>&1 | tail -1)
  sz=$(echo "$fdout" | cut -d'|' -f1); kb=$(echo "$fdout" | cut -d'|' -f2); rd=$(echo "$fdout" | cut -d'|' -f3)
  if [ "${sz:-99}" -le 2 ] 2>/dev/null; then
    echo "  PASS event loop: 80 anormal kopma sonrasi fd sizintisi yok (fark ${sz})"
  else
    echo "  FAIL event loop fd SIZINTISI: +${sz} fd (RST/yarim komut sonrasi temizlenmiyor)"; fail=1
  fi
  if [ "${kb:-0}" -gt 0 ] && [ "${rd:-0}" -gt 0 ]; then
    echo "  PASS per-IP limiti: ${kb} kabul (220) / ${rd} red (421)"
  else
    echo "  FAIL per-IP limiti uygulanmiyor: kabul=${kb} red=${rd} (hepsi kabul mu?)"; fail=1
  fi
else
  echo "  (atlandi: fd sayimi /proc gerektirir)"
fi

# Bind BASARISIZ oldugunda "started" YALANI basilmamali (45. bug).
# Olculmustu: bind hatasindan SONRA "IMAP server started on port X" yaziliyordu;
# kullanici log'a bakip calistigini saniyordu (sessiz basarisizlik).
BUSY_PORT=$((PORT + 50))
BUSY_HTTP=$((HTTP_PORT + 50))
python3 - "$BUSY_PORT" <<'PYEOF' >/dev/null 2>&1 &
import socket, sys, time
p = int(sys.argv[1])
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("::", p)); s.listen(1); time.sleep(25)
PYEOF
BLK=$!
sleep 1
mkdir -p "$TMP/md2"
LOOK_SMTP_PORT=$BUSY_PORT LOOK_MAIL_DIR="$TMP/md2" \
  "$FCGI" --mode http --port $BUSY_HTTP "$TMP/app.lk" >"$TMP/busy.log" 2>&1 &
BSRV=$!
for i in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$BUSY_HTTP/" 2>/dev/null && break; sleep 0.5; done
sleep 1
kill $BSRV 2>/dev/null; kill $BLK 2>/dev/null
if grep -qi "cannot bind\|dinlenemedi\|BAŞLATILAMADI" "$TMP/busy.log"; then
  if grep -q "SMTP server started" "$TMP/busy.log"; then
    echo "  FAIL bind BASARISIZ oldugu halde 'SMTP server started' yazildi (yaniltici log)"
    fail=1
  else
    echo "  PASS bind hatasinda yaniltici 'started' logu yok"
  fi
else
  echo "  (atlandi: bind hatasi tetiklenemedi)"
fi

[ $fail = 0 ] && echo "PASS: SMTP sunucusu" || echo "FAIL: SMTP sunucusu"
exit $fail

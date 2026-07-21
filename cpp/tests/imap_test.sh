#!/bin/bash
# IMAP sunucusu guard'i — imap_server.cpp (1013 satir) icin ILK guard.
#
# NEDEN: disariya port dinliyor, kimlik dogrulama yapiyor, guard'i YOKTU.
# Bu dosya YALNIZCA duzeltilmis/dogrulanmis davranisi kilitler; bilinen
# eksikler (UID komutu, UID kaliciligi, CREATE/DELETE, BODY[HEADER.FIELDS])
# BUG_AVI_HARITASI 7d'de ACIK olarak kayitli — guard'a konmadi ki yaniltmasin.
#
# Kilitlenenler:
#  1) Kimlik dogrulama zorunlulugu (auth'suz SELECT/FETCH/APPEND reddedilmeli)
#  2) Mailbox adi dogrulama (43. bug): `INBOX; rm -rf /` KABUL ediliyordu;
#     ayirici/`;`/kontrol karakteri iceren ad artik red. Var OLMAYAN mailbox
#     `OK (0 EXISTS)` donuyordu — RFC 3501 §6.3.1: `NO` olmali.
#  3) Traversal (auth SONRASI da)
#  4) APPEND literal siniri ILAN EDILIP ZORLANIYOR mu (LOOK_IMAP_MAX_LITERAL)
set -u
FCGI="${1:-./build/lk-fcgi}"
IMAP_PORT="${2:-7157}"
HTTP_PORT=$((IMAP_PORT + 700))
TMP=$(mktemp -d)
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail=0
command -v python3 >/dev/null 2>&1 || { echo "  (atlandi: python3 gerekli)"; exit 0; }

echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
mkdir -p "$TMP/md"
LOOK_IMAP_PORT=$IMAP_PORT LOOK_MAIL_DIR="$TMP/md" \
  LOOK_MAIL_USER="u@localhost" LOOK_MAIL_PASS="s123" \
  "$FCGI" --mode http --port $HTTP_PORT "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null && break; sleep 0.5; done
sleep 1

cat > "$TMP/p.py" <<'PYEOF'
import socket, sys, time
PORT = int(sys.argv[1])
def yeni():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10); s.settimeout(10); return s
def rd(s):
    try:
        d = s.recv(65536).decode("utf-8","replace"); time.sleep(0.06)
        s.setblocking(False)
        try:
            while True:
                e = s.recv(65536)
                if not e: break
                d += e.decode("utf-8","replace")
        except Exception: pass
        s.setblocking(True); s.settimeout(10); return d
    except Exception: return ""
def kod(cevap, tag):
    # "<tag> OK/NO/BAD ..." satirindaki durumu dondur
    for satir in cevap.replace("\r\n","\n").split("\n"):
        if satir.startswith(tag + " "):
            return satir.split(" ")[1]
    return "?"
sonuc = []
# 1) auth'suz erisim
s = yeni(); rd(s)
for i, k in enumerate(["SELECT INBOX", "FETCH 1 BODY[]", "APPEND INBOX {10}"]):
    t = "b%d" % i
    s.sendall(("%s %s\r\n" % (t, k)).encode()); sonuc.append(kod(rd(s), t))
s.close()
# 2) auth sonrasi mailbox dogrulama
s = yeni(); rd(s)
s.sendall(b"c0 LOGIN u@localhost s123\r\n"); sonuc.append(kod(rd(s), "c0"))
testler = [
    ("c1", "SELECT INBOX"),                    # OK  — pozitif kontrol
    ("c2", 'SELECT "INBOX; rm -rf /"'),        # NO  — 43. bug
    ("c3", 'SELECT "Yok Boyle"'),              # NO  — var olmayan mailbox
    ("c4", 'SELECT "../../etc"'),              # NO  — traversal
    ("c5", 'SELECT "/etc/passwd"'),            # NO  — mutlak yol
    ("c6", "APPEND INBOX {2147483647}"),       # NO  — literal siniri
    ("c7", "APPEND INBOX {-5}"),               # BAD — bozuk literal
    ("c8", "SELECT INBOX"),                    # OK  — hala saglam
]
for t, k in testler:
    s.sendall(("%s %s\r\n" % (t, k)).encode()); sonuc.append(kod(rd(s), t))
s.close()
print("|".join(sonuc))
PYEOF

out=$(timeout 90 python3 "$TMP/p.py" $IMAP_PORT 2>&1 | tail -1)
bek="NO|NO|NO|OK|OK|NO|NO|NO|NO|NO|BAD|OK"
if [ "$out" = "$bek" ]; then
  echo "  PASS IMAP: auth zorunlu + mailbox dogrulama + traversal + literal siniri"
else
  echo "  FAIL IMAP yanit dizisi: [$out]"
  echo "       beklenen          : [$bek]"
  echo "       sira: authsuz(SELECT|FETCH|APPEND) login INBOX 'rm-rf' yok-mailbox traversal mutlak literal-2GB literal-negatif INBOX-tekrar"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: IMAP sunucusu" || echo "FAIL: IMAP sunucusu"
exit $fail

#!/bin/bash
# IMAP server guard — the FIRST guard for imap_server.cpp (1013 lines).
#
# WHY: it listens on a port to the outside, does authentication, and had NO guard.
# This file locks ONLY fixed/verified behavior; known
# gaps (UID command, UID persistence, CREATE/DELETE, BODY[HEADER.FIELDS])
# were not put into the guard so it does not mislead.
#
# Locked down:
#  1) Authentication required (SELECT/FETCH/APPEND without auth must be rejected)
#  2) Mailbox name validation (bug 43): `INBOX; rm -rf /` was ACCEPTED;
#     a name containing a separator/`;`/control character is now rejected. A NON-EXISTENT mailbox
#     returned `OK (0 EXISTS)` — RFC 3501 §6.3.1: it must be `NO`.
#  3) Traversal (also AFTER auth)
#  4) Is the APPEND literal limit ADVERTISED AND ENFORCED (LOOK_IMAP_MAX_LITERAL)
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
    # return the status on the "<tag> OK/NO/BAD ..." line
    for satir in cevap.replace("\r\n","\n").split("\n"):
        if satir.startswith(tag + " "):
            return satir.split(" ")[1]
    return "?"
sonuc = []
# 1) access without auth
s = yeni(); rd(s)
for i, k in enumerate(["SELECT INBOX", "FETCH 1 BODY[]", "APPEND INBOX {10}"]):
    t = "b%d" % i
    s.sendall(("%s %s\r\n" % (t, k)).encode()); sonuc.append(kod(rd(s), t))
s.close()
# 2) mailbox validation after auth
s = yeni(); rd(s)
s.sendall(b"c0 LOGIN u@localhost s123\r\n"); sonuc.append(kod(rd(s), "c0"))
testler = [
    ("c1", "SELECT INBOX"),                    # OK  — positive control
    ("c2", 'SELECT "INBOX; rm -rf /"'),        # NO  — bug 43
    ("c3", 'SELECT "Yok Boyle"'),              # NO  — non-existent mailbox
    ("c4", 'SELECT "../../etc"'),              # NO  — traversal
    ("c5", 'SELECT "/etc/passwd"'),            # NO  — absolute path
    ("c6", "APPEND INBOX {2147483647}"),       # NO  — literal limit
    ("c7", "APPEND INBOX {-5}"),               # BAD — malformed literal
    ("c8", "FETCH 1 (UID)"),                   # NO  — no persistent UID, a loud error instead of silent corruption
    ("c9", "UID FETCH 1 BODY[]"),              # NO  — the UID command is not in M1's scope
    ("c10", "CREATE Sent"),                    # NO  — not in M1's scope
    ("c12", "FETCH 1 (BODY[HEADER.FIELDS (FROM)])"),  # BAD — unsupported item: used to be a SILENT empty FETCH
    ("c13", "FETCH 1 BODY.PEEK[]"),            # BAD — same class
    ("c14", "FETCH 1 (FLAGS BODY[HEADER])"),   # OK  — supported items keep working
    ("c11", "SELECT INBOX"),                   # OK  — still intact
]
for t, k in testler:
    s.sendall(("%s %s\r\n" % (t, k)).encode()); sonuc.append(kod(rd(s), t))
s.close()
print("|".join(sonuc))
PYEOF

out=$(timeout 90 python3 "$TMP/p.py" $IMAP_PORT 2>&1 | tail -1)
bek="NO|NO|NO|OK|OK|NO|NO|NO|NO|NO|BAD|NO|NO|NO|BAD|BAD|OK|OK"
if [ "$out" = "$bek" ]; then
  echo "  PASS IMAP: auth zorunlu + mailbox dogrulama + traversal + literal siniri"
else
  echo "  FAIL IMAP yanit dizisi: [$out]"
  echo "       beklenen          : [$bek]"
  echo "       sira: authsuz(SELECT|FETCH|APPEND) login INBOX 'rm-rf' yok-mailbox traversal mutlak literal-2GB literal-negatif UID-veri UID-komut CREATE HEADER.FIELDS PEEK desteklenen INBOX-tekrar"
  fail=1
fi

[ $fail = 0 ] && echo "PASS: IMAP sunucusu" || echo "FAIL: IMAP sunucusu"
exit $fail

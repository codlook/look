#!/bin/bash
# SMTP late-reply guard — the delivery reply must not be written to the WRONG connection.
#
# BUG (measured, 4/4): when the worker finished a message it wrote the "250 2.6.0 Message accepted"
# reply to the raw fd NUMBER at the moment DATA ended. If the client disconnected while the worker
# was busy, that fd was closed and a new connection that came in got the same number → the reply
# went to an UNRELATED connection, right after the 220 banner; that client's command/reply
# pairing was shifted by one.
#
# HOLDING THE WORKER: in the real bug a slow DNS (SPF) held the worker; the guard CANNOT DEPEND ON
# DNS. A 20 MB message is used: validating it and writing it to disk takes time in the worker.
# The sender finishes the message and sees the server close the connection (EOF), then new
# connections are opened and get that fd number. MAIL FROM:<> is used → SPF never asks DNS.
# (Until 1.0.17 a STARTTLS handshake left half-done was used; STARTTLS is not offered as of
# 1.0.18.)
#
# POSITIVE CONTROLS (so the guard cannot pass silently):
#  0) with NO holding, delivery gets the 250 on its own connection (the fix does not drop the legitimate reply),
#  1) when the new connections get their banner the big message must NOT YET be delivered; this is
#     measured in every attempt, an attempt that could not hold does not count; without two held attempts the test is RED,
#  2) the message is delivered at the end (the reply moment really happened),
#  3) the new connection later gets exactly its own reply to NOOP.
#
# Usage: bash smtp_late_reply_test.sh <lk-fcgi_path> [port]
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

# 0) No holding: the reply must arrive on its own connection.
k = conn(); zarf(k)
y = rd(k, 15)
if not y.startswith("250 2.6.0"): bitir("FAIL: control delivery got [%s], expected 250 2.6.0" % y.strip())
k.close()
taban = teslim()
if taban != 1: bitir("FAIL: control delivery wrote %d files, expected 1" % taban)

# 1) HOLD the worker: validating a big message and writing it to disk takes time in the worker.
#    (Until 1.0.17 a STARTTLS handshake left half-done was used; STARTTLS is no longer
#    offered.) Whether the hold happened is MEASURED in every attempt: when the new connections
#    get their banner the message must not yet be delivered. An attempt that could not hold does not count and is retried.
SATIR = b"X" * 76 + b"\r\n"
GOVDE = b"Subject: BIG\r\n\r\n" + SATIR * (20 * 1024 * 1024 // len(SATIR)) + b".\r\n"

def dene():
    taban = teslim()
    a = conn()
    if not rd(a).startswith("220"): bitir("FAIL: no 220 banner")
    cmd(a, "EHLO t"); cmd(a, "MAIL FROM:<>"); cmd(a, "RCPT TO:<u@localhost>")
    if not cmd(a, "DATA").startswith("354"): bitir("FAIL: DATA was not accepted")
    # 2) A: finish the message and drop WITHOUT waiting for the reply. See the server close the fd (EOF):
    #    so the next accept gets that number.
    a.sendall(GOVDE); a.shutdown(socket.SHUT_WR)
    a.settimeout(20)
    try:
        while a.recv(4096): pass
    except Exception: pass
    a.close()
    # 3) B: gets the fd number A left behind. Sends nothing.
    b = [conn() for _ in range(3)]
    for s in b:
        if not rd(s).startswith("220"): bitir("FAIL: new connection got no 220 banner")
    tutuldu = (teslim() == taban)
    # 4) The worker finishes: the message is delivered and its reply is about to be sent.
    for _ in range(120):
        if teslim() > taban: break
        time.sleep(0.25)
    else:
        bitir("FAIL: the message of the disconnected sender was never delivered")
    # 5) Nothing must arrive at B; then NOOP must get its own reply.
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

#!/usr/bin/env bash
# Every capability the SMTP and IMAP servers advertise works end to end — and nothing is
# advertised that this test does not check.
#
# Two SMTP capabilities were advertised and did not work: PIPELINING lost mail (fixed in
# 1.0.17) and STARTTLS completed the handshake and then never answered, so a sender using TLS
# could not deliver (measured on 1.0.16 and 1.0.17; no longer advertised from 1.0.18). Neither
# had a test. This one reads the advertised lists and fails on a keyword it has no check for.
# Usage: mail_capabilities_test.sh <lk-fcgi> [base port]
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [base port]}")" && pwd)/$(basename "$1")"
BASE="${2:-7860}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  (skipped: openssl is required)"; exit 0; }
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -keyout "$TMP/key.pem" -out "$TMP/cert.pem" >/dev/null 2>&1 || { echo "FAIL: could not create a test certificate"; exit 1; }
echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
cat > "$TMP/caps.py" <<'PY'
import socket, ssl, sys, time, glob, base64
sport, subport, iport, itls, maildir, cert = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6] == "cert"
bad = 0
def check(ok, what, got=""):
    global bad
    if ok: print("  OK   " + what)
    else: bad += 1; print("  FAIL %s -- got: %s" % (what, str(got)[:110]))
def rd(s, t=3.0, stop=None):
    s.settimeout(0.4); d = b""; end = time.time() + t
    while time.time() < end:
        try:
            x = s.recv(65536)
            if not x: break
            d += x
            if stop and stop in d: break
        except (socket.timeout, ssl.SSLWantReadError):
            if d and not stop: break
        except Exception as e: d += ("(%s)" % type(e).__name__).encode(); break
    return d.decode("utf-8", "replace")
def one(s, c, t=3.0):
    s.sendall(c); l = [x for x in rd(s, t).split("\r\n") if x]; return l[-1] if l else "(nothing)"
def files(): return [open(p, "rb").read() for p in glob.glob(maildir + "/**/new/*", recursive=True)]
def wait_for(marker):
    for i in range(24):
        if any(marker in f for f in files()): return True
        time.sleep(0.25)
    return False
def ehlo(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5); rd(s); s.sendall(b"EHLO t\r\n"); r = rd(s)
    return s, set(x[4:].split(" ")[0] for x in r.split("\r\n")[1:] if x.startswith("250"))
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
tag = "with a certificate" if cert else "without a certificate"
UTF8_FROM = b"MAIL FROM:<g\xc3\xb6nderen@\xc3\xb6rnek.test> SMTPUTF8\r\n"
UTF8_RCPT = b"RCPT TO:<\xc3\xbc@localhost>\r\n"

# ---- SMTP, port 25
s, caps = ehlo(sport)
known = {"SIZE", "8BITMIME", "PIPELINING", "SMTPUTF8", "OK"}
check(caps == known, "[SMTP %s] advertises exactly SIZE 8BITMIME PIPELINING SMTPUTF8 (each checked below)" % tag, sorted(caps))
check(one(s, b"STARTTLS\r\n").startswith("502"), "[SMTP] STARTTLS is not advertised and is refused with 502")
check(one(s, b"MAIL FROM:<> SIZE=999999999\r\n").startswith("552"), "[SMTP SIZE] a declared size over the limit is refused before the body"); one(s, b"RSET\r\n")
check(one(s, b"MAIL FROM:<> SIZE=100\r\n").startswith("250"), "[SMTP SIZE] a declared size within the limit is accepted"); one(s, b"RSET\r\n")
s.sendall(b"MAIL FROM:<> BODY=8BITMIME\r\nRCPT TO:<u@localhost>\r\nDATA\r\n"); r = rd(s, 4, b"354")
check([x[:3] for x in r.split("\r\n") if x] == ["250", "250", "354"], "[SMTP PIPELINING] MAIL, RCPT and DATA in one packet are answered in order", r)
check(one(s, b"Subject: EIGHT\r\n\r\n\xc3\xa7\xc4\x9f\xff\xfe raw\r\n.\r\n", 6).startswith("250"), "[SMTP 8BITMIME] a body with bytes above 127 is accepted")
check(wait_for(b"Subject: EIGHT") and any(b"\xc3\xa7\xc4\x9f\xff\xfe raw" in f for f in files()), "[SMTP 8BITMIME] the bytes are stored unchanged")
a = one(s, UTF8_FROM); b = one(s, UTF8_RCPT); c = one(s, b"DATA\r\n"); d = one(s, b"Subject: UTF8BOX\r\n\r\nx\r\n.\r\n", 6)
check(a.startswith("250") and b.startswith("250") and c.startswith("354") and d.startswith("250") and wait_for(b"Subject: UTF8BOX"), "[SMTP SMTPUTF8] a message with UTF-8 addresses is accepted and delivered", [a, b, c, d])
s.close()

# ---- SMTP, submission port
s, caps = ehlo(subport)
cred = base64.b64encode(b"\0u@localhost\0tok123")
if cert:
    check(caps == known, "[SMTP submission, certificate] AUTH is not advertised (it cannot succeed without encryption)", sorted(caps))
    check(one(s, b"AUTH PLAIN " + cred + b"\r\n").startswith("530"), "[SMTP submission, certificate] AUTH in plain text is refused")
else:
    check(caps == known | {"AUTH"}, "[SMTP submission] advertises AUTH", sorted(caps))
    check(one(s, b"MAIL FROM:<>\r\n").startswith("530"), "[SMTP AUTH] MAIL before AUTH is refused")
    check(one(s, b"AUTH PLAIN " + base64.b64encode(b"\0u@localhost\0wrong") + b"\r\n").startswith("535"), "[SMTP AUTH] a wrong password is refused")
    check(one(s, b"AUTH PLAIN " + cred + b"\r\n").startswith("235"), "[SMTP AUTH] the right password is accepted")
    check(one(s, b"MAIL FROM:<u@localhost>\r\n").startswith("250"), "[SMTP AUTH] MAIL after AUTH is accepted")
    # AUTH PLAIN/LOGIN only encodes the password. Without encryption it is offered to this
    # machine only: a connection that arrives from any other address must not be offered AUTH
    # and must be refused, with the right password too (1.0.18 accepted it from anywhere).
    lan = None
    try:
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.connect(("192.0.2.1", 9)); lan = u.getsockname()[0]; u.close()
    except Exception: pass
    if lan and not lan.startswith("127."):
        r2 = socket.create_connection((lan, subport), timeout=5); rd(r2); r2.sendall(b"EHLO t\r\n"); e2 = rd(r2)
        check("AUTH" not in e2, "[SMTP AUTH] not advertised to a connection from another address (%s)" % lan, e2)
        check(one(r2, b"AUTH PLAIN " + cred + b"\r\n").startswith("530"), "[SMTP AUTH] refused without encryption from another address, with the right password too")
        check(one(r2, b"MAIL FROM:<u@localhost>\r\n").startswith("530"), "[SMTP AUTH] and that connection cannot send")
        r2.close()
    else:
        check(False, "[SMTP AUTH] this machine has no non-loopback address, the refusal cannot be tested", lan)
s.close()

# ---- IMAP
s = socket.create_connection(("127.0.0.1", iport), timeout=5); g = rd(s)
icaps = set(g.split("[CAPABILITY ")[1].split("]")[0].split(" ")) if "[CAPABILITY " in g else set()
if cert:
    check(icaps == {"IMAP4rev1", "IDLE", "STARTTLS", "LOGINDISABLED"}, "[IMAP certificate] advertises exactly IMAP4rev1 IDLE STARTTLS LOGINDISABLED", sorted(icaps))
    check(" NO " in one(s, b"a0 LOGIN u@localhost s123\r\n"), "[IMAP LOGINDISABLED] LOGIN before TLS is refused")
    check(" OK " in one(s, b"a1 STARTTLS\r\n"), "[IMAP STARTTLS] the command is accepted")
    try: t = ctx.wrap_socket(s); hs = True
    except Exception as e: t = None; hs = False
    check(hs, "[IMAP STARTTLS] the handshake completes")
else:
    check(icaps == {"IMAP4rev1", "IDLE"}, "[IMAP] advertises exactly IMAP4rev1 IDLE", sorted(icaps)); t = s
if t:
    check("a3 OK" in one(t, b"a3 LOGIN u@localhost s123\r\n"), "[IMAP] LOGIN works on the session")
    check("a4 OK" in one(t, b"a4 SELECT INBOX\r\n"), "[IMAP] SELECT works on the session")
    t.sendall(b"a5 FETCH 1 (BODY[HEADER])\r\n"); r = rd(t, 4, b"a5 ")
    check("Subject: EIGHT" in r and "a5 OK" in r, "[IMAP] FETCH returns the message delivered over SMTP", r[-80:])
    t.sendall(b"a6 IDLE\r\n"); r = rd(t, 3, b"+ ")
    check(r.startswith("+"), "[IMAP IDLE] the server enters idle", r)
    t.sendall(b"DONE\r\n"); check("a6 OK" in rd(t, 4, b"a6 "), "[IMAP IDLE] DONE ends it")
    one(t, b"a7 LOGOUT\r\n")
if cert:
    try:
        t = ctx.wrap_socket(socket.create_connection(("127.0.0.1", itls), timeout=5)); g = rd(t)
        check("IMAP4rev1" in g and "STARTTLS" not in g and "b1 OK" in one(t, b"b1 LOGIN u@localhost s123\r\n"), "[IMAPS] implicit TLS: greeting, no STARTTLS offered, LOGIN works", g)
    except Exception as e: check(False, "[IMAPS] implicit TLS session", type(e).__name__)
print("RESULT bad=%d" % bad)
PY
fail=0
for mode in cert nocert; do
    rm -rf "$TMP/md"; mkdir -p "$TMP/md"
    extra=""; [ $mode = cert ] && extra="LOOK_SMTP_CERT=$TMP/cert.pem LOOK_SMTP_KEY=$TMP/key.pem LOOK_IMAP_PORT_TLS=$((BASE + 3))"
    ( exec env $extra LOOK_SMTP_PORT=$BASE LOOK_SMTP_SUB_PORT=$((BASE + 1)) LOOK_IMAP_PORT=$((BASE + 2)) LOOK_MAIL_DIR="$TMP/md" LOOK_SMTP_LOCAL_DOMAINS=localhost LOOK_MAIL_USER="u@localhost" LOOK_MAIL_PASS="s123" LOOK_SMTP_AUTH_TOKEN="tok123" "$FCGI" --mode http --port $((BASE + 100)) "$TMP/app.lk" > "$TMP/log.txt" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$((BASE + 100))/" && break; sleep 0.2; done; sleep 0.7
    out="$(python3 "$TMP/caps.py" $BASE $((BASE + 1)) $((BASE + 2)) $((BASE + 3)) "$TMP/md" $mode 2>&1)"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=""
    echo "$out" | grep -v "^RESULT"
    echo "$out" | grep -qx "RESULT bad=0" || fail=1
done
[ $fail = 0 ] && echo "PASS: every advertised mail capability works" || { echo "FAIL: an advertised mail capability does not work, or one is advertised without a check"; exit 1; }

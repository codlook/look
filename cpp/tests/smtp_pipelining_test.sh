#!/usr/bin/env bash
# The SMTP server handles commands sent one after another without waiting for the replies.
#
# It advertises PIPELINING (RFC 2920), which tells the client it may do exactly that — a
# mail server such as Postfix then sends MAIL, RCPT and DATA in one packet. Up to 1.0.16 the
# server answered the first command and left the rest in its buffer until more data arrived;
# the body sent next was read as commands. Commands sent right after the final "." were added
# to the message that had just ended and thrown away. Measured on the published 1.0.16: of
# five messages, the one with a pipelined envelope and the one sent right after a "." were
# lost without an error.
# Usage: smtp_pipelining_test.sh <lk-fcgi> [port]
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7841}"; HPORT=$((PORT + 100))
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
mkdir -p "$TMP/md"
echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
cat > "$TMP/client.py" <<'PY'
import socket, sys, time, glob
port, maildir = int(sys.argv[1]), sys.argv[2]
def conn(): return socket.create_connection(("127.0.0.1", port), timeout=5)
def rd(s, t=4.0, until=None, stop=None):
    s.settimeout(0.4); d = b""; end = time.time() + t
    while time.time() < end:
        try:
            x = s.recv(4096)
            if not x: break
            d += x
            if until and d.count(b"\r\n") >= until: break
            if stop and stop in d: break
        except socket.timeout:
            if d and not until and not stop: break
    return [l for l in d.decode("utf-8", "replace").split("\r\n") if l]
def codes(lines): return " ".join(l[:3] for l in lines if len(l) > 3 and l[3] != "-")
def msg(n): return ("Subject: M%d\r\n\r\nbody %d\r\n.\r\n" % (n, n)).encode()
ENV = b"MAIL FROM:<>\r\nRCPT TO:<u@localhost>\r\nDATA\r\n"   # MAIL FROM:<> — no SPF lookup, no DNS
# A: one command at a time, two messages on one connection
s = conn(); rd(s); s.sendall(b"EHLO t\r\n"); rd(s); out = []
for n in (1, 2):
    for c in (b"MAIL FROM:<>\r\n", b"RCPT TO:<u@localhost>\r\n", b"DATA\r\n"): s.sendall(c); out += rd(s, 3, 1)
    s.sendall(msg(n)); out += rd(s, 6, 1)
s.sendall(b"QUIT\r\n"); out += rd(s, 2, 1); s.close()
print("A=" + codes(out))
# B: the envelope in one packet, then the body
s = conn(); rd(s); s.sendall(b"EHLO t\r\n"); rd(s)
s.sendall(ENV); r = rd(s, 4, 3); s.sendall(msg(3)); r += rd(s, 6, 1); s.sendall(b"QUIT\r\n"); r += rd(s, 2, 1); s.close()
print("B=" + codes(r))
# C: the next envelope right after the final dot, in the same packet
s = conn(); rd(s); s.sendall(b"EHLO t\r\n"); rd(s)
for c in (b"MAIL FROM:<>\r\n", b"RCPT TO:<u@localhost>\r\n", b"DATA\r\n"): s.sendall(c); rd(s, 3, 1)
s.sendall(msg(4) + ENV); r = rd(s, 6, 4); s.sendall(msg(5)); r += rd(s, 6, 1); s.sendall(b"QUIT\r\n"); r += rd(s, 2, 1); s.close()
print("C=" + codes(r))
# D: a whole session in one packet
s = conn(); rd(s); s.sendall(b"EHLO t\r\n" + ENV + msg(6) + ENV + msg(7) + b"QUIT\r\n"); r = rd(s, 12, None, b"221 "); s.close()
print("D=" + codes(r))
got = set()
for i in range(20):
    got = set(l.strip().replace("Subject: ", "") for f in glob.glob(maildir + "/**/new/*", recursive=True) for l in open(f, errors="replace") if l.startswith("Subject:"))
    if len(got) >= 7: break
    time.sleep(0.5)
print("delivered=" + " ".join(sorted(got)))
PY
LOOK_SMTP_PORT=$PORT LOOK_MAIL_DIR="$TMP/md" LOOK_SMTP_LOCAL_DOMAINS=localhost "$FCGI" --mode http --port $HPORT "$TMP/app.lk" > "$TMP/log.txt" 2>&1 & pid=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 "localhost:$HPORT/" && break; sleep 0.2; done; sleep 0.5
out="$(python3 "$TMP/client.py" "$PORT" "$TMP/md" 2>&1)"
fail=0
check() { if echo "$out" | grep -qx "$1"; then echo "  OK   $2"; else echo "  FAIL $2 — got: $(echo "$out" | grep "^${1%%=*}=")"; fail=1; fi; }
check "A=250 250 354 250 250 250 354 250 221" "one command at a time: two messages on one connection"
check "B=250 250 354 250 221"                 "MAIL, RCPT and DATA in one packet"
check "C=250 250 250 354 250 221"             "the next envelope right after the final dot"
check "D=250 250 250 354 250 250 250 354 250 221" "a whole two-message session in one packet"
check "delivered=M1 M2 M3 M4 M5 M6 M7"        "all seven messages are in the Maildir"
[ $fail = 0 ] && echo "PASS: SMTP pipelining" || { echo "FAIL: SMTP pipelining — commands sent without waiting were lost"; exit 1; }

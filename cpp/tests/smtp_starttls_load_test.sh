#!/usr/bin/env bash
# SMTP STARTTLS with TLS 1.3 when the client's first command follows its Finished within a few hundred microseconds.
#
# Found by a mail test that went red only on a busy machine. An independent client (openssl s_client) saw
# "decryption failed or bad record mac" in 3-6% of the sessions on a loaded machine and in none on an idle one,
# and none with TLS 1.2. The cause: the event loop keeps reading the socket until it is empty. A client that sends
# its Finished and its first command (EHLO) one right after the other had that command processed while the
# handshake output (the two TLS 1.3 session tickets) was still waiting to be written; the reply was queued with
# async_write, which keeps ONE pending write per descriptor, and REPLACED the tickets. The client's first record was
# then the reply, with the wrong sequence number, and the session dropped. (The server trace showed it:
# "async_write size=108 pending_before=478" in every bad session, in no good one.)
#
# Load is not needed to make that happen — only the timing: the second piece has to arrive after the server has read
# the first one and before the server has finished with it. This test builds the TLS 1.3 client itself (Python,
# ssl.MemoryBIO), sends the Finished record and then the encrypted EHLO as two TCP segments with a random gap of
# 0-600 microseconds, and reads the answer. Any session that ends in a decryption error, a closed connection or no
# capability list fails. On a build without the fix about one session in ten is bad on an idle machine.
# Usage: smtp_starttls_load_test.sh <lk-fcgi> [port] [sessions]     (needs python3 with TLS 1.3, openssl, curl)
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port] [sessions]}")" && pwd)/$(basename "$1")"
BASE="${2:-7870}"; SESSIONS="${3:-400}"
command -v python3 >/dev/null 2>&1 || { echo "  (skipped: python3 is required)"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  (skipped: openssl is required)"; exit 0; }
python3 -c 'import ssl; assert ssl.HAS_TLSv1_3' 2>/dev/null || { echo "  (skipped: this python has no TLS 1.3)"; exit 0; }
TMP="$(mktemp -d)"; pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$TMP"' EXIT
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -keyout "$TMP/key.pem" -out "$TMP/cert.pem" >/dev/null 2>&1 \
    || { echo "FAIL: could not create a test certificate"; exit 1; }
echo 'route("GET","/",function(){ return response::text("ok") })' > "$TMP/app.lk"
cat > "$TMP/client.py" <<'PY'
import socket, ssl, sys, time, random
port, n = int(sys.argv[1]), int(sys.argv[2])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
try:
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
except Exception:
    ctx.options |= getattr(ssl, "OP_NO_TLSv1_2", 0) | getattr(ssl, "OP_NO_TLSv1_1", 0) | getattr(ssl, "OP_NO_TLSv1", 0)

def read_until(s, marker):
    """plain text until a line that starts with marker (SMTP: the last line of a reply)"""
    d = b""
    while True:
        x = s.recv(65536)
        if not x: return None
        d += x
        if any(l.startswith(marker) for l in d.split(b"\r\n")): return d

def session(gap_us):
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        if read_until(s, b"220 ") is None: return "no banner"
        s.sendall(b"EHLO t\r\n")
        if read_until(s, b"250 ") is None: return "no EHLO answer"
        s.sendall(b"STARTTLS\r\n")
        if read_until(s, b"220 ") is None: return "no STARTTLS answer"
        inc, out = ssl.MemoryBIO(), ssl.MemoryBIO()
        so = ctx.wrap_bio(inc, out)
        while True:
            try:
                so.do_handshake(); break
            except ssl.SSLWantReadError:
                d = out.read()
                if d: s.sendall(d)
                x = s.recv(65536)
                if not x: return "closed during the handshake"
                inc.write(x)
        fin = out.read()                    # change-cipher-spec + Finished
        so.write(b"EHLO t\r\n"); app = out.read()   # the first command, encrypted
        s.sendall(fin)
        t0 = time.perf_counter()
        while (time.perf_counter() - t0) * 1e6 < gap_us: pass
        s.sendall(app)
        plain = b""
        while b"PIPELINING" not in plain:
            x = s.recv(65536)
            if not x: return "connection closed before the capability list (got %r)" % plain[:60]
            inc.write(x)
            while True:
                try:
                    y = so.read(65536)
                    if not y: break
                    plain += y
                except ssl.SSLWantReadError:
                    break
        return None
    except ssl.SSLError as e:
        return "TLS error: %s" % str(e)[:100]
    except socket.timeout:
        return "timeout"
    finally:
        s.close()

random.seed(20261123)
bad = 0; first = ""
for i in range(n):
    r = session(random.uniform(0, 600))
    if r:
        bad += 1
        if not first: first = "session %d: %s" % (i + 1, r)
print("RESULT bad=%d of %d %s" % (bad, n, first))
sys.exit(1 if bad else 0)
PY
( cd "$TMP" && exec env LOOK_SMTP_CERT="$TMP/cert.pem" LOOK_SMTP_KEY="$TMP/key.pem" LOOK_SMTP_PORT=$BASE \
    LOOK_SMTP_SUB_PORT=$((BASE + 1)) LOOK_IMAP_PORT=$((BASE + 2)) LOOK_MAIL_DIR="$TMP/md" LOOK_SMTP_LOCAL_DOMAINS=localhost \
    "$FCGI" --mode http --port $((BASE + 100)) app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$((BASE + 100))/" && break; sleep 0.2; done; sleep 0.5
out="$(python3 "$TMP/client.py" $BASE "$SESSIONS" 2>&1)"; rc=$?
echo "$out" | tail -3
if [ $rc = 0 ]; then echo "  OK   $SESSIONS STARTTLS sessions with the first command right behind the Finished: none dropped or garbled"
else echo "  FAIL STARTTLS sessions were dropped or garbled when the first command followed the Finished closely"; fi
[ $rc = 0 ] && echo "PASS: SMTP STARTTLS, first command right behind the Finished" || { echo "FAIL: SMTP STARTTLS, first command right behind the Finished"; exit 1; }

#!/usr/bin/env bash
# mail::send (MAIL_PROVIDER=smtp) ile uzak SMTP sunucusu: TLS ve kimlik doğrulama koşulları.
# Bu istemcinin (mail_stdlib.cpp send_smtp) hiç testi yoktu — yalnız LOOK'un kendi SMTP sunucusu
# sınanıyordu. Gerçek lk + sahte bir SMTP sunucusu (Python, STARTTLS / doğrudan TLS); sunucu TLS'in
# ne zaman kurulduğunu ve AUTH / parolanın ne zaman geldiğini kaydeder:
#   - güvenilen sertifikayla STARTTLS: ileti gider, AUTH TLS'ten SONRA
#   - uzak sunucu kimlik doğrulamayı reddeder (535): açık hata; parola hiçbir çıktıda yok
#   - kendinden imzalı / başka ada verilmiş sertifika: reddedilir, AUTH hiç gönderilmez
#   - sunucu STARTTLS sunmaz: düz metne DÜŞÜLMEZ (indirgeme), AUTH gönderilmez
#   - doğrudan TLS (465 / SMTP_TLS=implicit) güvenilen sertifikayla çalışır
#   - SMTP_TLS=none: düz metin, günlükte açık uyarı
# Usage: smtp_client_tls_test.sh <lk> [base-port]   (needs python3, openssl)
set -u
LK="$(cd "$(dirname "${1:?usage: $0 <lk> [base-port]}")" && pwd)/$(basename "$1")"
P="${2:-7880}"
TMP="$(mktemp -d)"; spid=""; wd=""
trap '[ -n "$spid" ] && kill "$spid" 2>/dev/null; [ -n "$wd" ] && kill "$wd" 2>/dev/null; rm -rf "$TMP"' EXIT
( sleep 180; echo "FAIL: smtp client test timed out (180 s)"; kill -TERM $$ ) 2>/dev/null & wd=$!; disown $wd   # disown: öldürülünce kabuk "Terminated" basmasın
fail=0
check() { if [ "$2" = ok ]; then echo "  OK   $1"; else echo "  FAIL $1: $3"; fail=1; fi; }
PW="look-test-secret-pw-7731"

# ── sertifikalar: test CA'sı, localhost (SAN), başka ad, kendinden imzalı ──
cd "$TMP"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=LOOK Test CA" -keyout ca.key -out ca.pem >/dev/null 2>&1
leaf() { # <ad> <dosya>
    openssl req -newkey rsa:2048 -nodes -subj "/CN=$1" -keyout "$2.key" -out "$2.csr" >/dev/null 2>&1
    printf 'subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\n' "$1" > "$2.ext"
    openssl x509 -req -in "$2.csr" -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 -extfile "$2.ext" -out "$2.pem" >/dev/null 2>&1
}
leaf localhost good
leaf other.example wrongname
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" -keyout self.key -out self.pem >/dev/null 2>&1
[ -s good.pem ] && [ -s wrongname.pem ] && [ -s self.pem ] || { echo "FAIL: could not create test certificates"; exit 1; }
cd - >/dev/null

cat > "$TMP/server.py" <<'PY'
import socket, ssl, sys, threading
port, mode, cert, log = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
def note(s):
    with open(log, "a") as f: f.write(s + "\n")
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert + ".pem", cert + ".key")
def serve(c):
    f = None
    def send(s): c.sendall(s.encode())
    def lines():
        buf = b""
        while True:
            while b"\r\n" not in buf:
                d = c.recv(4096)
                if not d: return
                buf += d
            l, buf = buf.split(b"\r\n", 1)
            yield l.decode("latin-1")
    try:
        if mode == "implicit":
            c = ctx.wrap_socket(c, server_side=True); note("TLS")
        send("220 fake ESMTP\r\n")
        it = lines(); tls = mode == "implicit"
        for l in it:
            u = l.upper()
            if u.startswith("EHLO"):
                ext = [] if (tls or mode == "notls") else ["STARTTLS"]
                send("250-fake\r\n" + "".join("250-%s\r\n" % e for e in ext) + "250 AUTH LOGIN\r\n")
            elif u == "STARTTLS":
                if mode == "notls": send("502 not here\r\n"); continue
                send("220 go ahead\r\n")
                try:
                    c = ctx.wrap_socket(c, server_side=True); tls = True; note("TLS"); it = lines()
                except Exception as e:
                    note("TLSFAIL"); return
            elif u == "AUTH LOGIN":
                note("AUTH" + ("" if tls else " PLAIN-TEXT")); send("334 VXNlcm5hbWU6\r\n")
                user = next(it); send("334 UGFzc3dvcmQ6\r\n")
                pw = next(it); note("PASS" + ("" if tls else " PLAIN-TEXT"))
                if mode == "reject": send("535 5.7.8 Authentication credentials invalid\r\n")
                else: send("235 ok\r\n")
            elif u.startswith("MAIL FROM") or u.startswith("RCPT TO"): send("250 ok\r\n")
            elif u == "DATA":
                send("354 go\r\n")
                for d in it:
                    if d == ".": break
                note("DATA"); send("250 queued\r\n")
            elif u == "QUIT": send("221 bye\r\n"); return
            else: send("500 ?\r\n")
    except Exception as e:
        note("ERR " + type(e).__name__)
    finally:
        try: c.close()
        except Exception: pass
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(8)
print("ready", flush=True)
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
cat > "$TMP/send.lk" <<'LK'
use mail
use json
print(json::encode(mail::send("to@example.com", "Hello", "body text")))
LK
# <sunucu kipi> <sertifika> <SMTP_TLS> → $out (mail::send sonucu + stderr), $log (sunucu kaydı)
run() {
    : > "$TMP/srv.log"
    python3 "$TMP/server.py" "$P" "$1" "$TMP/$2" "$TMP/srv.log" > "$TMP/srv.out" 2>&1 & spid=$!
    for i in $(seq 1 50); do grep -q ready "$TMP/srv.out" && break; sleep 0.1; done
    out=$(cd "$TMP" && env -i PATH="$PATH" MAIL_PROVIDER=smtp SMTP_HOST=localhost SMTP_PORT="$P" SMTP_USER=user \
          SMTP_PASS="$PW" SMTP_TLS="$3" MAIL_FROM=from@example.com SSL_CERT_FILE="$TMP/ca.pem" \
          timeout 30 "$LK" "$TMP/send.lk" 2>&1)
    sleep 0.3; log=$(tr '\n' ' ' < "$TMP/srv.log")
    kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null; spid=""
    P=$((P + 1))
}
nopw() { case "$out" in *"$PW"*|*"$(printf '%s' "$PW" | base64)"*) return 1 ;; esac; return 0; }

echo "== STARTTLS with a trusted certificate"
run starttls good starttls
check "the message is delivered ($out)" "$( [[ "$out" == *'"ok":true'* ]] && [[ "$log" == *DATA* ]] && echo ok)" "$out | $log"
check "AUTH is sent only after TLS ($log)" "$( [[ "$log" == "TLS AUTH PASS DATA "* ]] && echo ok)" "$log"

echo "== the server refuses the login (535)"
run reject good starttls
check "mail::send fails with a clear message" "$( [[ "$out" == *'"ok":false'* ]] && [[ "$out" == *"SMTP authentication failed"* ]] && echo ok)" "$out"
check "the password is in no output" "$(nopw && echo ok)" "password printed"
check "nothing is sent after the refusal" "$( [[ "$log" != *DATA* ]] && echo ok)" "$log"

echo "== a self-signed certificate"
run starttls self starttls
check "is refused ($out)" "$( [[ "$out" == *'"ok":false'* ]] && [[ "$out" == *"STARTTLS handshake failed"* ]] && echo ok)" "$out"
check "the login is never sent" "$( [[ "$log" != *AUTH* ]] && [[ "$log" != *PASS* ]] && echo ok)" "$log"

echo "== a certificate for another name"
run starttls wrongname starttls
check "is refused" "$( [[ "$out" == *'"ok":false'* ]] && [[ "$out" == *"handshake failed"* ]] && echo ok)" "$out"
check "the login is never sent" "$( [[ "$log" != *AUTH* ]] && echo ok)" "$log"

echo "== the server does not offer STARTTLS"
run notls good ""
check "mail::send does not fall back to plain text ($out)" "$( [[ "$out" == *'"ok":false'* ]] && [[ "$out" == *"STARTTLS refused"* ]] && echo ok)" "$out"
check "the login is never sent" "$( [[ "$log" != *AUTH* ]] && [[ "$log" != *PASS* ]] && echo ok)" "$log"

echo "== implicit TLS (SMTP_TLS=implicit)"
run implicit good implicit
check "the message is delivered over TLS" "$( [[ "$out" == *'"ok":true'* ]] && [[ "$log" == "TLS AUTH PASS DATA "* ]] && echo ok)" "$out | $log"
run implicit self implicit
check "a self-signed certificate is refused there too" "$( [[ "$out" == *'"ok":false'* ]] && [[ "$log" != *AUTH* ]] && echo ok)" "$out | $log"

echo "== SMTP_TLS=none"
run notls good none
check "plain text works when chosen" "$( [[ "$out" == *'"ok":true'* ]] && [[ "$log" == *"AUTH PLAIN-TEXT"* ]] && echo ok)" "$out | $log"
check "with a warning that says so" "$( [[ "$out" == *"clear text"* ]] && echo ok)" "$out"
check "the password is in no output" "$(nopw && echo ok)" "password printed"

[ $fail = 0 ] && echo "PASS: mail::send over SMTP keeps TLS and the login safe" || echo "FAIL: mail::send over SMTP"
exit $fail

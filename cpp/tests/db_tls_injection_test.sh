#!/usr/bin/env bash
# STARTTLS-style injection on the database connections: a server (or someone in between) sends
# plain-text bytes right before the TLS handshake — after the MySQL greeting, or after the
# PostgreSQL 'S' answer to SSLRequest. A client that kept those bytes in its read buffer would
# read them after the handshake as if they had come over TLS (PostgreSQL CVE-2021-23222 class).
# LOOK must refuse the connection. A positive control (same server, no extra bytes) shows the
# refusal comes from the injected bytes and not from a broken fake server.
# Usage: db_tls_injection_test.sh <lk> [port]   (needs python3)
set -u
LK="$(cd "$(dirname "${1:?usage: db_tls_injection_test.sh <lk> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-55471}"
T="$(mktemp -d)"; srv=""; trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/srv.py" <<'PY'
# Sahte MySQL / PG: TLS'e geçmeden hemen önce düz metin bayt ekler (inject) ya da eklemez (clean).
import socket, struct, sys
proto, mode, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(4)
print("ready", flush=True)
while True:
    c, _ = s.accept()
    try:
        if proto == "mysql":
            caps = 0x0008 | 0x0200 | 0x0800 | 0x2000 | 0x8000 | 0x80000   # ... CLIENT_SSL ...
            g = (b"\x0a" + b"8.0.0-fake\x00" + struct.pack("<I", 7) + b"abcdefgh" + b"\x00" +
                 struct.pack("<H", caps & 0xFFFF) + bytes([45]) + struct.pack("<H", 2) +
                 struct.pack("<H", caps >> 16) + bytes([21]) + b"\x00" * 10 + b"ijklmnopqrst\x00" +
                 b"mysql_native_password\x00")
            pkt = struct.pack("<I", len(g))[:3] + b"\x00" + g
            extra = (struct.pack("<I", 7)[:3] + b"\x02" + b"\x00\x00\x00\x02\x00\x00\x00") if mode == "inject" else b""
            c.sendall(pkt + extra)              # selamlama + (inject) sahte OK, aynı gönderimde
        else:
            c.recv(8)                           # SSLRequest
            extra = (b"R" + struct.pack("!II", 8, 0) + b"Z" + struct.pack("!I", 5) + b"I") if mode == "inject" else b""
            c.sendall(b"S" + extra)             # 'S' + (inject) sahte AuthenticationOk + ReadyForQuery
        c.settimeout(2)
        try: c.recv(4096)                       # istemcinin TLS ClientHello'su (ya da kapanışı)
        except OSError: pass
    finally:
        c.close()
PY
fail=0
run() {   # proto mode dsn → LOOK çıktısı
    python3 "$T/srv.py" "$1" "$2" "$PORT" > "$T/srv.log" 2>&1 & srv=$!
    for i in $(seq 1 50); do grep -q ready "$T/srv.log" && break; sleep 0.1; done
    printf 'use db\ntry { $c = db::connect("%s"); print("CONNECTED") } catch ($e) { print("ERR " . $e) }\n' "$3" > "$T/t.lk"
    out="$(cd "$T" && timeout 20 "$LK" t.lk 2>&1 | grep -v '^\[20' | tail -1)"
    kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=""
    echo "$out"
}
for proto in mysql pg; do
    if [ $proto = mysql ]; then dsn="mysql://u:p@127.0.0.1:$PORT/d?tls=insecure"; want="before TLS started"
    else dsn="postgres://u:p@127.0.0.1:$PORT/d?tls=insecure"; want="before TLS started"; fi
    inj="$(run $proto inject "$dsn")"
    clean="$(run $proto clean "$dsn")"
    if echo "$inj" | grep -qi "$want"; then echo "  OK   [$proto] bytes injected before TLS: refused ($inj)"
    else echo "  FAIL [$proto] bytes injected before TLS: $inj"; fail=1; fi
    # pozitif kontrol: eklenmemiş bayt → ret nedeni enjeksiyon değil (el sıkışma hatası)
    if echo "$clean" | grep -qi "$want"; then echo "  FAIL [$proto] control without injected bytes also says injection: $clean"; fail=1
    else echo "  OK   [$proto] control without injected bytes fails for another reason ($clean)"; fi
done
[ $fail = 0 ] && echo "PASS: bytes sent before the TLS handshake are refused (MySQL, PostgreSQL)" || { echo "FAIL: pre-TLS injection"; exit 1; }

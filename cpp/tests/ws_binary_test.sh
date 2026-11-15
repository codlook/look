#!/usr/bin/env bash
# WebSocket ikili mesajlar (include/look/websocket.h, http_main.cpp ws::*).
# ESKİ: ws::send her mesajı metin çerçevesiyle gönderiyordu — ikili veri (geçersiz UTF-8) gidince
# tarayıcı bağlantıyı kapatıyordu, uygulama nedenini göremiyordu; gelen ikili mesaj metinden ayırt
# edilemiyordu. Şimdi: ws::send_binary / ws::broadcast_binary ikili çerçeve (0x2) gönderir; ws::send ve
# ws::broadcast geçerli UTF-8 olmayan metinde açık hata verir; işleyici fn($data, $binary) ikili mi
# olduğunu alır (tek parametreli eski işleyici aynı çalışır); sınır metin ve ikili için aynı (16 MB).
# Usage: ws_binary_test.sh <lk-fcgi> [port]
FCGI="$(cd "$(dirname "${1:?usage: $0 <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7748}"
TMP="$(mktemp -d)"; wd=""
trap 'rm -rf "$TMP"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null; [ -n "$wd" ] && kill "$wd" 2>/dev/null' EXIT
# Takılırsa 180 sn'de kırmızı; bekçi çıkışta öldürülür (bitmiş testin PID'ine sinyal gitmesin)
( sleep 180; echo "FAIL: ws binary test timed out (180 s)"; kill -TERM $$ ) 2>/dev/null & wd=$!
cat > "$TMP/app.lk" <<'LK'
route("GET", "/ok", fn() => response::text("ok"))
// ikili geri ikili, metin geri "text:" + metin
route("WS", "/echo", function($ws) {
    ws::on($ws, "message", function($m, $bin) {
        if ($bin) { ws::send_binary($ws, $m) } else { ws::send($ws, "text:" . $m) }
    })
})
// tek parametreli (eski) işleyici
route("WS", "/old", function($ws) {
    ws::on($ws, "message", function($m) { ws::send($ws, "old:" . strlen($m)) })
})
// gelen veriyi metin olarak geri göndermeyi dener
route("WS", "/astext", function($ws) {
    ws::on($ws, "message", function($m) {
        try { ws::send($ws, $m); ws::send($ws, "sent") } catch ($e) { ws::send($ws, "error:" . $e) }
    })
})
route("WS", "/bcast", function($ws) {
    ws::on($ws, "message", function($m, $bin) {
        if ($bin) { ws::broadcast_binary($m) }
        else { try { ws::broadcast($m) } catch ($e) { ws::send($ws, "error:" . $e) } }
    })
})
LK
cat > "$TMP/client.py" <<'PY'
import socket, base64, os, sys, struct, time
PORT = int(sys.argv[1])
# Beklenmedik hata (ör. eski sunucu yanıt vermedi) döküm yerine FAIL satırı olsun
sys.excepthook = lambda t, v, tb: print("  FAIL unexpected %s: %s" % (t.__name__, v))
fails = 0
def check(name, ok, why=""):
    global fails
    print(("  OK   " if ok else "  FAIL ") + name + ("" if ok else ": " + why)); fails += (0 if ok else 1)
def connect(path):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, PORT, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = s.recv(4096)
        if not c: raise Exception("handshake closed")
        buf += c
    assert b" 101 " in buf.split(b"\r\n")[0], buf[:80]
    return s
def send_frame(s, opcode, payload, fin=True):
    mask = os.urandom(4)
    hdr = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126: hdr += bytes([0x80 | n])
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload)) if n < 1 << 20 else \
             (int.from_bytes(payload, "big") ^ int.from_bytes((mask * (n // 4 + 1))[:n], "big")).to_bytes(n, "big")
    s.sendall(hdr + mask + masked)
def recvn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError()
        b += c
    return b
def recv_frame(s):
    h = recvn(s, 2); op = h[0] & 0x0F; n = h[1] & 0x7F
    if n == 126: n = struct.unpack(">H", recvn(s, 2))[0]
    elif n == 127: n = struct.unpack(">Q", recvn(s, 8))[0]
    return op, recvn(s, n)
BIN = bytes([0x00, 0xFF, 0xFE, 0x80, 0xC0, 0x41, 0x0A]) + os.urandom(300)   # geçersiz UTF-8 + rastgele

s = connect("/echo")
send_frame(s, 0x2, BIN); op, p = recv_frame(s)
check("a binary message comes back as a binary frame with the same bytes", op == 2 and p == BIN, "op=%d len=%d" % (op, len(p)))
send_frame(s, 0x1, "hello".encode()); op, p = recv_frame(s)
check("a text message is text, and the handler is told so", op == 1 and p == b"text:hello", "op=%d %r" % (op, p[:40]))
send_frame(s, 0x2, BIN[:100], fin=False); send_frame(s, 0x0, BIN[100:], fin=True); op, p = recv_frame(s)
check("a binary message in two parts arrives whole, as binary", op == 2 and p == BIN, "op=%d len=%d" % (op, len(p)))
send_frame(s, 0x1, "par".encode(), fin=False); send_frame(s, 0x0, "ts".encode(), fin=True); op, p = recv_frame(s)
check("a text message in two parts arrives whole, as text", op == 1 and p == b"text:parts", "op=%d %r" % (op, p[:40]))
big = os.urandom(1 << 20)
send_frame(s, 0x2, big); op, p = recv_frame(s)
check("a 1 MiB binary message goes both ways", op == 2 and p == big, "op=%d len=%d" % (op, len(p)))
s.close()

s = connect("/old")
send_frame(s, 0x2, BIN); op, p = recv_frame(s)
check("a handler with one parameter works as before", op == 1 and p == ("old:%d" % len(BIN)).encode(), repr(p[:40]))
s.close()

s = connect("/astext")
send_frame(s, 0x2, BIN); op, p = recv_frame(s)
want = b"error:ws::send(): the message is not valid UTF-8 text; send binary data with ws::send_binary()"
check("ws::send of bytes that are not UTF-8 is an error that names ws::send_binary", op == 1 and p == want, repr(p[:120]))
utf = "çğüş €😀".encode()
send_frame(s, 0x1, utf); op, p = recv_frame(s); op2, p2 = recv_frame(s)
check("valid UTF-8 text (several bytes per character) is sent", op == 1 and p == utf and p2 == b"sent", repr(p[:40]))
s.close()

a = connect("/bcast"); b = connect("/bcast"); time.sleep(0.2)
send_frame(a, 0x2, BIN); op, p = recv_frame(b)
check("ws::broadcast_binary reaches another client as binary", op == 2 and p == BIN, "op=%d len=%d" % (op, len(p)))
recv_frame(a)   # a da yayını alır
send_frame(a, 0x1, b"\xff\xfe"); op, p = recv_frame(a)   # geçersiz UTF-8 metin → ws::broadcast reddeder
want = b"error:ws::broadcast(): the message is not valid UTF-8 text; send binary data with ws::broadcast_binary()"
check("ws::broadcast of bytes that are not UTF-8 is an error that names ws::broadcast_binary", op == 1 and p == want, repr(p[:120]))
send_frame(a, 0x1, b"ok"); recv_frame(a); recv_frame(b)
a.close(); b.close()

def closed_after(frames):
    s = connect("/echo")
    try:
        for f in frames: send_frame(s, *f)
        s.settimeout(10)
        op, p = recv_frame(s)
        return op == 8
    except (EOFError, ConnectionResetError, BrokenPipeError):
        return True
    finally:
        s.close()
over = os.urandom((16 << 20) + 1)
check("a binary message over 16 MB in one frame closes the connection", closed_after([(0x2, over)]))
half = (8 << 20) + 1
check("a binary message over 16 MB in parts closes the connection",
      closed_after([(0x2, over[:half], False), (0x0, over[half:], True)]))
s = connect("/echo"); send_frame(s, 0x2, b"\x01\x02"); op, p = recv_frame(s)
check("after that the server still answers", op == 2 and p == b"\x01\x02", "op=%d" % op)
s.close()
sys.exit(1 if fails else 0)
PY
LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}" "$FCGI" --mode http --port "$PORT" "$TMP/app.lk" > "$TMP/srv.log" 2>&1 & pid=$!
for i in $(seq 1 50); do [ "$(curl -s -m 1 "http://127.0.0.1:$PORT/ok")" = ok ] && break; sleep 0.2; done
python3 "$TMP/client.py" "$PORT"; rc=$?
kill -0 "$pid" 2>/dev/null || { echo "  FAIL the server died"; tail -5 "$TMP/srv.log"; rc=1; }
[ $rc = 0 ] && echo "PASS: WebSocket binary messages" || { echo "FAIL: WebSocket binary messages"; tail -5 "$TMP/srv.log"; }
exit $rc

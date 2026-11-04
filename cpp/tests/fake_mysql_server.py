#!/usr/bin/env python3
# Sahte MySQL sunucusu — LOOK'un MySQL istemcisine bozuk bir cevap gönderir.
# Mod "desync": FIRST içeren ifadenin EXECUTE cevabı sınırı aşan bir sütun sayısıyla başlar
# (istemci hata atar), ardından cevabın geri kalanı (sütun tanımı, EOF, satır, EOF) gelir. Öteki
# ifadeler tek sütun, tek satır döndürür: değer ifadedeki addır (SECOND → "second").
# İstemci yarım kalan cevabı atmazsa okunmamış paketler sonraki sorgunun cevabı sanılır.
import socket, struct, sys, threading

MODE = sys.argv[1] if len(sys.argv) > 1 else "desync"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 33099

def pkt(seq, body):
    return struct.pack("<I", len(body))[:3] + bytes([seq & 0xFF]) + body

def lenenc_str(s):
    b = s.encode()
    return bytes([len(b)]) + b

def ok(seq):
    return pkt(seq, b"\x00\x00\x00\x02\x00\x00\x00")

def eof(seq):
    return pkt(seq, b"\xfe\x00\x00\x02\x00")

def coldef(seq):
    body = (lenenc_str("def") + lenenc_str("") + lenenc_str("") + lenenc_str("") +
            lenenc_str("col") + lenenc_str("") + b"\x0c" + struct.pack("<H", 45) +
            struct.pack("<I", 255) + b"\xfd" + b"\x00\x00" + b"\x00" + b"\x00\x00")
    return pkt(seq, body)

def recv_pkt(c):
    h = b""
    while len(h) < 4:
        x = c.recv(4 - len(h))
        if not x: return None, None
        h += x
    n = h[0] | (h[1] << 8) | (h[2] << 16)
    body = b""
    while len(body) < n:
        x = c.recv(n - len(body))
        if not x: return None, None
        body += x
    return h[3], body

def handle(c):
    caps = 0x0008 | 0x0200 | 0x2000 | 0x8000 | 0x80000   # CONNECT_WITH_DB, PROTOCOL_41, TRANSACTIONS, SECURE_CONN, PLUGIN_AUTH
    greet = (b"\x0a" + b"8.0.0-fake\x00" + struct.pack("<I", 7) + b"abcdefgh" + b"\x00" +
             struct.pack("<H", caps & 0xFFFF) + bytes([45]) + struct.pack("<H", 2) +
             struct.pack("<H", caps >> 16) + bytes([21]) + b"\x00" * 10 + b"ijklmnopqrst\x00" +
             b"mysql_native_password\x00")
    c.sendall(pkt(0, greet))
    seq, _ = recv_pkt(c)            # istemcinin kimlik cevabı — her şey kabul
    if seq is None: return
    c.sendall(ok(seq + 1))
    stmts, next_id = {}, 1
    while True:
        seq, body = recv_pkt(c)
        if body is None or not body: return
        cmd = body[0]
        if cmd == 0x01: return                           # COM_QUIT
        if cmd in (0x03, 0x0e):                          # COM_QUERY (SET SESSION …), COM_PING
            c.sendall(ok(1)); continue
        if cmd == 0x19: stmts.pop(struct.unpack("<I", body[1:5])[0], None); continue   # STMT_CLOSE
        if cmd == 0x16:                                  # STMT_PREPARE
            sid = next_id; next_id += 1
            stmts[sid] = body[1:].decode("utf-8", "replace").upper()
            c.sendall(pkt(1, b"\x00" + struct.pack("<IHH", sid, 1, 0) + b"\x00" + b"\x00\x00") + coldef(2) + eof(3))
            continue
        if cmd == 0x17:                                  # STMT_EXECUTE
            sql = stmts.get(struct.unpack("<I", body[1:5])[0], "")
            name = "first" if "FIRST" in sql else ("second" if "SECOND" in sql else "third")
            row = pkt(4, b"\x00" + b"\x00" + lenenc_str(name))
            if MODE == "desync" and name == "first":
                head = pkt(1, b"\xfd" + struct.pack("<I", 70000)[:3])   # 70000 sütun (sınır 65535)
            else:
                head = pkt(1, b"\x01")
            c.sendall(head + coldef(2) + eof(3) + row + eof(5))
            continue
        c.sendall(ok(1))

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT)); s.listen(5)
print("sahte-mysql hazir mod=%s port=%d" % (MODE, PORT), flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

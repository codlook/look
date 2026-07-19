#!/usr/bin/env python3
# Sahte PostgreSQL sunucusu — LOOK'un wire ayristiricisina DUSMANCA mesaj gonderir.
# Amac: bozuk DataRow'da LOOK sessizce bos veri mi donduruyor, yoksa hata mi veriyor?
import socket, struct, sys, threading

MODE = sys.argv[1] if len(sys.argv) > 1 else "truncated"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 55432

def msg(t, body):            # tip + uzunluk(kendisi dahil) + govde
    return t + struct.pack("!I", len(body) + 4) + body

def cstr(s):
    return s.encode() + b"\x00"

def row_desc(name="col"):
    b = struct.pack("!H", 1)                 # 1 alan
    b += cstr(name)
    b += struct.pack("!IHIhih", 0, 0, 25, -1, -1, 0)   # oid=25 (text)
    return msg(b"T", b)

def data_row(mode):
    if mode == "truncated":
        # UZUNLUK YALAN SOYLUYOR: 100 bayt diyor, 2 bayt veriyor
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 100) + b"ab")
    if mode == "negatif":
        # -5: NULL (-1) degil ama negatif — tanimsiz bolge
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", -5) + b"ab")
    if mode == "dev_uzunluk":
        # int32 max: isaretci tasmasi denemesi
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 2147483647) + b"ab")
    if mode == "eksik_alan":
        # 3 alan vaat ediyor, 1 tane veriyor
        return msg(b"D", struct.pack("!H", 3) + struct.pack("!i", 2) + b"ab")
    if mode == "saglam":
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 2) + b"ab")
    raise SystemExit("bilinmeyen mod")

def handle(c):
    # Startup paketi (tip baytI YOK): uzunluk + surum + key/value
    hdr = c.recv(4)
    if len(hdr) < 4: return
    n = struct.unpack("!I", hdr)[0]
    c.recv(n - 4)
    c.sendall(msg(b"R", struct.pack("!I", 0)))        # AuthenticationOk
    c.sendall(msg(b"Z", b"I"))                        # ReadyForQuery
    while True:
        t = c.recv(1)
        if not t: return
        ln = struct.unpack("!I", c.recv(4))[0]
        c.recv(ln - 4)
        if t == b"Q":
            c.sendall(row_desc())
            c.sendall(data_row(MODE))
            c.sendall(msg(b"C", cstr("SELECT 1")))
            c.sendall(msg(b"Z", b"I"))
        elif t == b"X":
            return

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT)); s.listen(5)
print("sahte-pg hazir mod=%s port=%d" % (MODE, PORT), flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

#!/usr/bin/env python3
# Düşmanca MySQL / PostgreSQL sunucusu — wire ayrıştırıcıları için mutasyon fuzz'u.
# Her sorguya GEÇERLİ ama zengin bir cevap kurar (çok sütun/tür, NULL, çok satır, bazen ek sonuç
# kümesi ya da satır arası hata), sonra tohumlu rastgele bozar (bayt çevirme, kırpma, uzatma,
# paket silme/çoğaltma, uzunluk alanını bozma, tür baytını değiştirme), gönderir ve bağlantıyı
# kapatır (istemci beklemeye kalmasın: her bozuk cevap EOF ile biter). Sayaç dosyası yok; tohum
# bağlantı sırasıdır — aynı çalıştırma aynı dizileri üretir.
#   Kullanım: db_wire_mutator.py mysql|pg <port> [tohum]
import socket, struct, sys, threading, random

PROTO = sys.argv[1]
PORT = int(sys.argv[2])
BASE_SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
_n = [0]; _nl = threading.Lock()
def next_rng():
    with _nl:
        _n[0] += 1
        return random.Random(BASE_SEED * 1000003 + _n[0])

def mutate(rng, chunks):
    """chunks: bayt dizgelerinin listesi (paket/mesaj). Bozulmuş liste döner."""
    chunks = list(chunks)
    for _ in range(rng.choice([0, 1, 1, 2, 3, 5])):
        if not chunks: break
        i = rng.randrange(len(chunks)); c = bytearray(chunks[i]); op = rng.randrange(9)
        if op == 0 and c: c[rng.randrange(len(c))] ^= 1 << rng.randrange(8)
        elif op == 1 and c: c = c[:rng.randrange(len(c))]                          # kırp
        elif op == 2: c += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 40)))  # uzat
        elif op == 3: del chunks[i]; continue                                      # paketi sil
        elif op == 4: chunks.insert(i, bytes(c))                                   # çoğalt
        elif op == 5 and len(c) > 4: c[rng.randrange(4)] = rng.choice([0, 1, 0x7f, 0xff, rng.randrange(256)])  # başlık/uzunluk
        elif op == 6 and len(c) > 4: c[4 if len(c) > 4 else 0] = rng.choice([0x00, 0xfb, 0xfc, 0xfd, 0xfe, 0xff])
        elif op == 7 and c: c[rng.randrange(len(c))] = rng.choice([0x00, 0xff, 0x80, 0xfe])
        elif op == 8: chunks.insert(i, bytes(rng.randrange(256) for _ in range(rng.randrange(1, 12))))
        chunks[i] = bytes(c)
    return chunks

# ── MySQL ────────────────────────────────────────────────────────────────────
def mpkt(seq, body): return struct.pack("<I", len(body))[:3] + bytes([seq & 0xFF]) + body
def lstr(b):
    if isinstance(b, str): b = b.encode()
    if len(b) < 251: return bytes([len(b)]) + b
    if len(b) < 65536: return b"\xfc" + struct.pack("<H", len(b)) + b
    return b"\xfd" + struct.pack("<I", len(b))[:3] + b
MTYPES = [0x08, 0x05, 0x04, 0x0c, 0x0b, 0x0a, 0xfd, 0x01, 0x03, 0x02, 0xfc]
def mcoldef(name, t):
    return (lstr("def") + lstr("d") + lstr("t") + lstr("t") + lstr(name) + lstr(name) + b"\x0c" +
            struct.pack("<H", 45) + struct.pack("<I", 255) + bytes([t]) + b"\x00\x00\x00\x00\x00")
def mval(rng, t):
    if t == 0x08: return struct.pack("<q", rng.randrange(-2**63, 2**63))
    if t == 0x05: return struct.pack("<d", rng.random() * 1e6)
    if t == 0x04: return struct.pack("<f", rng.random())
    if t in (0x0c, 0x0a): L = rng.choice([0, 4, 7, 11]); return bytes([L]) + struct.pack("<H", 2026) + bytes([11, 7, 13, 5, 9]) [:max(0, L - 2)] + b"\x01\x02\x03\x04"[:max(0, L - 7)]
    if t == 0x0b: L = rng.choice([0, 8, 12]); return bytes([L]) + bytes(rng.randrange(60) for _ in range(L))
    if t in (0x01,): return bytes([rng.randrange(256)])
    if t == 0x02: return struct.pack("<h", rng.randrange(-30000, 30000))
    if t == 0x03: return struct.pack("<i", rng.randrange(-2**31, 2**31))
    return lstr(bytes(rng.randrange(32, 127) for _ in range(rng.choice([0, 3, 50, 300]))))
def mysql_resultset(rng, seq, status_more):
    ncol = rng.randrange(1, 7); types = [rng.choice(MTYPES) for _ in range(ncol)]
    out = [mpkt(seq, lstr(b"")[:0] + bytes([ncol]))]; seq += 1
    for i, t in enumerate(types): out.append(mpkt(seq, mcoldef("c%d" % i, t))); seq += 1
    out.append(mpkt(seq, b"\xfe\x00\x00\x02\x00")); seq += 1
    for _ in range(rng.choice([0, 1, 3, 20])):
        nb = bytearray((ncol + 7 + 2) // 8); vals = b""
        for i, t in enumerate(types):
            if rng.random() < 0.15: nb[(i + 2) // 8] |= 1 << ((i + 2) % 8)
            else: vals += mval(rng, t)
        out.append(mpkt(seq, b"\x00" + bytes(nb) + vals)); seq += 1
    if rng.random() < 0.1:   # satırlar arasında hata
        out.append(mpkt(seq, b"\xff" + struct.pack("<H", 1242) + b"#21000Subquery returns more than 1 row")); seq += 1
    st = 0x0002 | (0x0008 if status_more else 0) | (0x0001 if rng.random() < 0.2 else 0)
    out.append(mpkt(seq, b"\xfe\x00\x00" + struct.pack("<H", st))); seq += 1
    return out, seq
def mysql_execute_reply(rng):
    more = rng.random() < 0.3
    out, seq = mysql_resultset(rng, 1, more)
    if more:   # CALL: ek küme(ler) ve son OK
        if rng.random() < 0.5:
            o2, seq = mysql_resultset(rng, seq, False); out += o2
        out.append(mpkt(seq, b"\x00\x00\x00" + struct.pack("<H", 0x0002) + b"\x00\x00"))
    return out
def mysql_handle(c):
    rng = next_rng()
    caps = 0x0008 | 0x0200 | 0x2000 | 0x8000 | 0x80000
    greet = (b"\x0a" + b"8.0.0-mut\x00" + struct.pack("<I", 7) + b"abcdefgh" + b"\x00" +
             struct.pack("<H", caps & 0xFFFF) + bytes([45]) + struct.pack("<H", 2) +
             struct.pack("<H", caps >> 16) + bytes([21]) + b"\x00" * 10 + b"ijklmnopqrst\x00" +
             b"mysql_native_password\x00")
    c.sendall(mpkt(0, greet))
    if mrecv(c) is None: return
    c.sendall(mpkt(2, b"\x00\x00\x00\x02\x00\x00\x00"))
    while True:
        p = mrecv(c)
        if p is None or not p: return
        cmd = p[0]
        if cmd == 0x01: return
        if cmd in (0x03, 0x0e): c.sendall(mpkt(1, b"\x00\x00\x00\x02\x00\x00\x00")); continue
        if cmd == 0x19: continue
        if cmd == 0x16:   # PREPARE: geçerli (0 parametre, sütun tanımları) — bazen bozuk
            ncol = rng.randrange(0, 4)
            rep = [mpkt(1, b"\x00" + struct.pack("<IHH", 1, ncol, 0) + b"\x00\x00\x00")]
            for i in range(ncol): rep.append(mpkt(2 + i, mcoldef("c%d" % i, 0xfd)))
            if ncol: rep.append(mpkt(2 + ncol, b"\xfe\x00\x00\x02\x00"))
            if rng.random() < 0.3:
                rep = mutate(rng, rep); c.sendall(b"".join(rep)); c.close(); return
            c.sendall(b"".join(rep)); continue
        if cmd == 0x17:
            c.sendall(b"".join(mutate(rng, mysql_execute_reply(rng)))); c.close(); return
        c.sendall(mpkt(1, b"\x00\x00\x00\x02\x00\x00\x00"))
def mrecv(c):
    h = b""
    while len(h) < 4:
        x = c.recv(4 - len(h))
        if not x: return None
        h += x
    n = h[0] | (h[1] << 8) | (h[2] << 16); b = b""
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x: return None
        b += x
    return b

# ── PostgreSQL ───────────────────────────────────────────────────────────────
def gmsg(t, body): return t + struct.pack("!I", len(body) + 4) + body
def cstr(s): return s.encode() + b"\x00"
POIDS = [23, 20, 701, 700, 25, 16, 1184, 1700, 17]
def pval(rng, oid):
    if rng.random() < 0.15: return struct.pack("!i", -1)
    v = {23: str(rng.randrange(-99999, 99999)), 20: str(rng.randrange(-2**63, 2**63)), 701: repr(rng.random()),
         700: "1.5", 25: "x" * rng.choice([0, 5, 300]), 16: rng.choice(["t", "f"]),
         1184: "2026-11-07 10:00:00+00", 1700: "12.50", 17: "\\x00ff"}[oid].encode()
    return struct.pack("!i", len(v)) + v
def pg_reply(rng):
    out = [gmsg(b"1", b""), gmsg(b"2", b"")]
    ncol = rng.randrange(1, 7); oids = [rng.choice(POIDS) for _ in range(ncol)]
    rd = struct.pack("!H", ncol)
    for i, o in enumerate(oids): rd += cstr("c%d" % i) + struct.pack("!IhIhih", 0, 0, o, -1, -1, 0)
    out.append(gmsg(b"T", rd))
    for _ in range(rng.choice([0, 1, 3, 20])):
        out.append(gmsg(b"D", struct.pack("!H", ncol) + b"".join(pval(rng, o) for o in oids)))
    if rng.random() < 0.1: out.append(gmsg(b"N", b"SNOTICE\x00Mhello\x00\x00"))
    if rng.random() < 0.1:
        out.append(gmsg(b"E", b"SERROR\x00C22012\x00Mdivision by zero\x00\x00"))
    elif rng.random() < 0.05:
        out.append(gmsg(rng.choice([b"G", b"H"]), b"\x00\x00\x01\x00\x00"))
    else:
        out.append(gmsg(b"C", cstr("SELECT 3")))
    out.append(gmsg(b"Z", rng.choice([b"I", b"T", b"E"])))
    return out
def pg_handle(c):
    rng = next_rng()
    h = c.recv(4)
    if len(h) < 4: return
    n = struct.unpack("!I", h)[0]; c.recv(n - 4)
    c.sendall(gmsg(b"R", struct.pack("!I", 0)) + gmsg(b"S", cstr("server_version") + cstr("16.0")) +
              gmsg(b"K", struct.pack("!II", 1, 2)) + gmsg(b"Z", b"I"))
    buf = b""
    while True:
        x = c.recv(65536)
        if not x: return
        buf += x
        if b"S\x00\x00\x00\x04" in buf:          # extended: Parse/Bind/Describe/Execute/Sync
            c.sendall(b"".join(mutate(rng, pg_reply(rng)))); c.close(); return
        if buf[:1] == b"Q" and len(buf) >= 5 and len(buf) >= 1 + struct.unpack("!I", buf[1:5])[0]:
            c.sendall(gmsg(b"C", cstr("BEGIN")) + gmsg(b"Z", b"I")); buf = b""
        if buf[:1] == b"X": return

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT)); s.listen(64)
print("mutator %s port=%d seed=%d" % (PROTO, PORT, BASE_SEED), flush=True)
H = mysql_handle if PROTO == "mysql" else pg_handle
while True:
    conn, _ = s.accept()
    def run(cc=conn):
        try: H(cc)
        except Exception: pass
        finally:
            try: cc.close()
            except Exception: pass
    threading.Thread(target=run, daemon=True).start()

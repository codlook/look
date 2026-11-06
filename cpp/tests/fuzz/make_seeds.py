#!/usr/bin/env python3
# Seed corpora for fuzz_gzip_inflate and fuzz_ws_frame (corpus_* directories are not in git).
# Usage: make_seeds.py <dir with corpus_gzip / corpus_ws to create>
import gzip, os, struct, sys, zlib

root = sys.argv[1] if len(sys.argv) > 1 else "."
g = os.path.join(root, "corpus_gzip"); w = os.path.join(root, "corpus_ws")
os.makedirs(g, exist_ok=True); os.makedirs(w, exist_ok=True)

samples = [b"", b"hello", b"A" * 5000, bytes((i * 7919) & 0xFF for i in range(3000)),
           b"{\"a\":[1,2,3],\"b\":\"x\"}" * 200, bytes(range(256)) * 40]
for i, s in enumerate(samples):
    open(os.path.join(g, "s%d.gz" % i), "wb").write(gzip.compress(s, compresslevel=(i % 9) + 1))
body = b"flags test" * 50
c = zlib.compressobj(6, zlib.DEFLATED, -15); raw = c.compress(body) + c.flush()
hdr = bytes([0x1f, 0x8b, 8, 0x1e, 0, 0, 0, 0, 0, 3]) + struct.pack("<H", 4) + b"xxxx" + b"name\0" + b"comment\0" + b"\0\0"
open(os.path.join(g, "flags.gz"), "wb").write(hdr + raw + struct.pack("<II", zlib.crc32(body) & 0xffffffff, len(body)))
c = zlib.compressobj(0, zlib.DEFLATED, -15); raw = c.compress(b"stored" * 100) + c.flush()
open(os.path.join(g, "stored.gz"), "wb").write(bytes([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3]) + raw + b"\0" * 8)

def frame(op, p, fin=True, masked=True):
    b0 = (0x80 if fin else 0) | op; m = b"\x11\x22\x33\x44"; n = len(p)
    if n < 126: h = bytes([b0, (0x80 if masked else 0) | n])
    elif n < 65536: h = bytes([b0, (0x80 if masked else 0) | 126]) + struct.pack(">H", n)
    else: h = bytes([b0, (0x80 if masked else 0) | 127]) + struct.pack(">Q", n)
    return h + (m if masked else b"") + bytes(x ^ m[i % 4] for i, x in enumerate(p))
seeds = [frame(1, b"hi"), frame(2, b"x" * 300), frame(1, b"y" * 70000), frame(1, b"a", fin=False) + frame(0, b"b"),
         frame(9, b"ping"), frame(8, b""), frame(1, b"hi") * 20, frame(1, b"un", masked=False)]
for i, s in enumerate(seeds):
    open(os.path.join(w, "f%d.bin" % i), "wb").write(s)
print("seeds:", len(os.listdir(g)), "gzip,", len(os.listdir(w)), "ws")

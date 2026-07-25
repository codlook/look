import socket, sys, time
PORT = int(sys.argv[1])
fail = 0
# Variant 1 — SUREKLI damla (idle'i sifirlar). deadline 2s. Damlalar 1s'de bir.
# FIX: ~3s'de deadline -> sunucu kapatir -> sonraki send BrokenPipe.
# FIX'siz: tum damlalar gecer (idle sifirlanir), baglanti TUTULUR -> recv HANG.
s = socket.socket(); s.connect(("127.0.0.1", PORT))
drip = [b"POST /e HTTP/1.1\r\n", b"H1: a\r\n", b"H2: b\r\n", b"H3: c\r\n",
        b"H4: d\r\n", b"H5: e\r\n", b"H6: f\r\n", b"H7: g\r\n"]
cut = False
try:
    for p in drip:
        s.sendall(p); time.sleep(1.0)
except (BrokenPipeError, ConnectionResetError, OSError):
    cut = True                       # sunucu damla ortasinda kapatti (FIX)
if not cut:
    s.settimeout(2.0)
    try:
        d = s.recv(1024)
        if d == b"" or b"408" in d: cut = True
    except socket.timeout:
        cut = False                  # TUTULUYOR (VULN)
    except Exception:
        cut = True
s.close()
print("  slowloris-surekli-damla -> %s  %s" % ("KESILDI" if cut else "TUTULUYOR", "PASS" if cut else "FAIL"))
if not cut: fail = 1
# hizli normal -> 200
s2 = socket.socket(); s2.settimeout(5); s2.connect(("127.0.0.1", PORT))
s2.sendall(b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello")
d2 = s2.recv(1024); s2.close()
ok2 = b"200" in d2
print("  hizli normal -> %s  %s" % ("200" if ok2 else "?", "PASS" if ok2 else "FAIL"))
if not ok2: fail = 1
sys.exit(fail)

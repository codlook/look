import socket, sys, time, re
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7601
def status(req):
    s = socket.socket(); s.settimeout(4)
    try:
        s.connect(("127.0.0.1", PORT)); s.sendall(req); time.sleep(0.3)
        data = s.recv(4096).decode("latin1","replace")
    except Exception: data = ""
    finally: s.close()
    m = re.match(r"HTTP/1\.1 (\d+)", data); return int(m.group(1)) if m else 0
def body_overshoot():
    # header ayri (body_in_buf=0), sonra body(10)+FAZLA -> body CL'yi asmamali
    s = socket.socket(); s.settimeout(4)
    try:
        s.connect(("127.0.0.1", PORT))
        s.sendall(b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n"); time.sleep(0.4)
        s.sendall(b"0123456789EXTRA-SMUGGLED"); time.sleep(0.4)
        data = s.recv(4096).decode("latin1","replace")
    except Exception: data = ""
    finally: s.close()
    m = re.search(r'"blen":(\d+)', data); return int(m.group(1)) if m else -1
CH = b"5\r\nhello\r\n0\r\n\r\n"
cases = [
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"+CH, 200, "TE chunked gecerli"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: xchunked\r\n\r\n"+CH, 400, "TE xchunked ret"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunkedx\r\n\r\n"+CH, 400, "TE chunkedx ret"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"+CH, 400, "TE chunked,gzip ret"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\nhello", 0, "CL bosluk-once-kolon ret"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", 200, "CL 5 gecerli"),
]
fail = 0
for req, exp, label in cases:
    c = status(req); ok = (c != 200) if exp == 0 else (c == exp)
    print("  %-34s exp=%s got=%s %s" % (label, exp, c, "PASS" if ok else "FAIL"))
    if not ok: fail = 1
# 55: body CL ust siniri
bl = body_overshoot(); ok = (bl == 10)
print("  %-34s exp=10 got=%s %s" % ("body CL-ustsinir (55)", bl, "PASS" if ok else "FAIL"))
if not ok: fail = 1
sys.exit(fail)

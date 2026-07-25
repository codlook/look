import socket, sys, time, re
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7601
def raw(req):
    s = socket.socket(); s.settimeout(4)
    try:
        s.connect(("127.0.0.1", PORT)); s.sendall(req)
        time.sleep(0.3); data = s.recv(4096).decode("latin1","replace")
    except Exception as e: data = ""
    finally: s.close()
    code = 0
    m = re.match(r"HTTP/1\.1 (\d+)", data)
    if m: code = int(m.group(1))
    return code
CH = b"5\r\nhello\r\n0\r\n\r\n"
cases = [
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"+CH, 200, "TE chunked gecerli"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: xchunked\r\n\r\n"+CH, 400, "TE xchunked reddedilmeli"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunkedx\r\n\r\n"+CH, 400, "TE chunkedx reddedilmeli"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"+CH, 400, "TE chunked,gzip reddedilmeli"),
    (b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\nhello", 0, "CL bosluk-once-kolon reddedilmeli"),  # 0=kapandi (200 OLMAMALI)
    (b"POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", 200, "CL 5 gecerli"),
]
fail = 0
for req, exp, label in cases:
    code = raw(req)
    if exp == 0:
        ok = (code != 200)   # reddedilmeli: 200 OLMAMALI (400 veya kapali)
    else:
        ok = (code == exp)
    print("  %-36s exp=%s got=%s %s" % (label, exp, code, "PASS" if ok else "FAIL"))
    if not ok: fail = 1
sys.exit(fail)

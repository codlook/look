import socket, threading, base64, os, struct, time, random, sys
HOST, PORT = "127.0.0.1", 7711
def ws_connect():
    s = socket.create_connection((HOST, PORT), timeout=4)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /ws HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: http://%s:%d\r\n\r\n"
           % (HOST, PORT, key, HOST, PORT))
    s.sendall(req.encode()); s.settimeout(4)
    resp = s.recv(256)
    if b"101" not in resp: raise RuntimeError("no 101: %r" % resp[:40])
    return s
def masked_text(msg):
    b = msg.encode(); m = os.urandom(4)
    payload = bytes(c ^ m[i%4] for i,c in enumerate(b))
    n = len(b)
    hdr = b"\x81" + bytes([0x80 | n]) + m   # FIN+text, masked, len<126
    return hdr + payload
def worker(tid):
    for it in range(12):
        try:
            s = ws_connect()
            for k in range(random.randint(1,4)):
                s.sendall(masked_text("m%d-%d" % (tid, k)))
                try: s.recv(256)
                except: pass
            # bazen close frame, bazen sert kapat (abandon)
            if random.random() < 0.5:
                s.sendall(b"\x88\x80" + os.urandom(4))  # masked close
            s.close()
        except Exception as e:
            pass
def bcaster():
    for _ in range(40):
        try:
            c = socket.create_connection((HOST,PORT),timeout=2)
            c.sendall(b"GET /bc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            c.recv(64); c.close()
        except: pass
        time.sleep(0.01)
ts = [threading.Thread(target=worker, args=(i,)) for i in range(40)]
ts += [threading.Thread(target=bcaster) for _ in range(3)]
for t in ts: t.start()
for t in ts: t.join()
print("LOAD DONE")

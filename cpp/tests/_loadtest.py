import socket, time, threading, sys
PORT=7401; N=30000; C=8
req=b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"  # keep-alive
cnt=[0]; lock=threading.Lock(); stop=[False]
def worker():
    try:
        s=socket.create_connection(("127.0.0.1",PORT),timeout=3); s.settimeout(3)
        buf=b""
        while not stop[0]:
            s.sendall(req)
            # bir yanit oku (Content-Length'e bakmadan, kaba: 'ok' gorunce say)
            while b"\r\n\r\n" not in buf:
                d=s.recv(4096)
                if not d: return
                buf+=d
            # basit: her tam yaniti tuket (header+body kucuk)
            buf=b""
            with lock:
                cnt[0]+=1
                if cnt[0]>=N: stop[0]=True; return
        s.close()
    except: return
best=0
for r in range(3):
    cnt[0]=0; stop[0]=False
    t0=time.time()
    ts=[threading.Thread(target=worker) for _ in range(C)]
    [t.start() for t in ts]; [t.join() for t in ts]
    dt=time.time()-t0; rps=cnt[0]/dt
    if rps>best: best=rps
print("%s: best-of-3 = %.0f req/s (keep-alive)"%(sys.argv[1] if len(sys.argv)>1 else "run", best))

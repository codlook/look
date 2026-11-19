import socket, struct, threading, time, sys
HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv)>1 else 7710
STOP = time.time() + (float(sys.argv[2]) if len(sys.argv)>2 else 6)
def churn():
    while time.time() < STOP:
        try:
            s = socket.create_connection((HOST,PORT), timeout=2)
            s.sendall(b"GET /sse HTTP/1.1\r\nHost: x\r\n\r\n")
            s.settimeout(0.2)
            try:
                for _ in range(2): s.recv(256)   # birkac ping al (timer donsun)
            except: pass
            # ANİDEN RST ile kapat (SO_LINGER 0) -> event-loop close_sse, timer hala send ediyor
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii',1,0))
            s.close()
        except: pass
ts=[threading.Thread(target=churn) for _ in range(40)]
[t.start() for t in ts]; [t.join() for t in ts]
print("SSE churn bitti")

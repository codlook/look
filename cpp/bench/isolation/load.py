# Keep-alive load: P processes x N requests each on one connection; prints requests/second.
import socket, sys, time, multiprocessing as mp
port, path, P, N = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
def run(_):
    s = socket.create_connection(("127.0.0.1", port)); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    req = ("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode(); buf = b""
    for _ in range(N):
        s.sendall(req)
        while True:
            i = buf.find(b"\r\n\r\n")
            if i >= 0:
                head = buf[:i].lower(); j = head.find(b"content-length:")
                need = i + 4 + int(head[j + 15:].split(b"\r\n")[0])
                if len(buf) >= need: buf = buf[need:]; break
            buf += s.recv(65536)
    s.close()
if __name__ == "__main__":
    t0 = time.time()
    with mp.Pool(P) as p: p.map(run, range(P))
    print(int(P * N / (time.time() - t0)))

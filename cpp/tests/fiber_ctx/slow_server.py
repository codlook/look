#!/usr/bin/env python3
# Deliberately slow HTTP responder: every request sleeps before replying, so a
# LOOK fiber that calls http::get() here stays suspended long enough for MANY
# other fibers to interleave and overwrite a shared web-context. Widening this
# yield window is what turns the latent fiber-context leak from "rare" to
# "every request".
import socket, threading, time, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7699
DELAY = float(sys.argv[2]) if len(sys.argv) > 2 else 0.4

def handle(c):
    try:
        c.recv(4096)
        time.sleep(DELAY)
        body = b"x"
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s"
                  % (len(body), body))
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT))
s.listen(256)
sys.stderr.write("slow_server on %d delay=%.2fs\n" % (PORT, DELAY)); sys.stderr.flush()
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

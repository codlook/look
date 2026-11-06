#!/usr/bin/env python3
# WebSocket backpressure on every platform (Linux epoll, Windows IOCP): a client flooding a slow
# handler is slowed by TCP once the connection's inbox is full, and other clients still get
# answers. Measured without /proc: the bytes the flooding client manages to hand to its socket in
# a few seconds stop growing (the server stopped reading). Before IOCP had pause_read the server
# read everything on Windows, so the inbox — and memory — grew with what the client sent.
#   Usage: ws_backpressure_portable_test.py <lk-fcgi[.exe]> <port>
import base64, os, socket, subprocess, sys, tempfile, time

exe, port = os.path.abspath(sys.argv[1]), int(sys.argv[2])
tmp = tempfile.mkdtemp()
app = os.path.join(tmp, "app.lk")
with open(app, "w") as f:
    f.write('route("WS", "/sink", function($ws) {\n'
            '    ws::on($ws, "message", function($m) { $n = 0\n'
            '        for ($i = 0; $i < 20000; $i++) { $n = $n + $i } })\n'
            '})\n'
            'route("WS", "/ws", function($ws) { ws::on($ws, "message", function($m) { ws::send($ws, "echo") }) })\n'
            'route("GET", "/ok", fn() => response::text("ok"))\n')
env = dict(os.environ, LOOK_SECRET=os.environ.get("LOOK_SECRET", "look-test-secret-0123456789abcdef0123456789"))
srv = subprocess.Popen([exe, "--mode", "http", "--port", str(port), "--workers", "2", app], cwd=tmp, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fail = 0
try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=1).close(); break
        except OSError: time.sleep(0.1)
    def hs(path):
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, base64.b64encode(os.urandom(16)).decode())).encode())
        b = b""
        while b"\r\n\r\n" not in b: b += s.recv(4096)
        return s
    def frame(p):
        m = os.urandom(4); hdr = bytes([0x81]) + (bytes([0x80 | len(p)]) if len(p) < 126 else bytes([0x80 | 126]) + len(p).to_bytes(2, "big"))
        return hdr + m + bytes(b ^ m[i % 4] for i, b in enumerate(p))
    def rss_mb():   # sunucunun yerleşik belleği — /proc olmadan da (Windows: GetProcessMemoryInfo)
        if os.name == "nt":
            import ctypes, ctypes.wintypes as w
            class PMC(ctypes.Structure):
                _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD)] + \
                           [(n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            h = ctypes.windll.kernel32.OpenProcess(0x0410, False, srv.pid)
            pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
            ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
            ctypes.windll.kernel32.CloseHandle(h)
            return pmc.WorkingSetSize / 1048576.0
        for l in open("/proc/%d/status" % srv.pid):
            if l.startswith("VmRSS:"): return int(l.split()[1]) / 1024.0
        return 0.0
    msg = b"".join(frame(b"x") for _ in range(2000))   # küçük çerçeve seli: her biri ayrı gelen-kutusu öğesi
    flood = hs("/sink"); flood.setblocking(False)
    echo = hs("/ws")
    m0 = rss_mb()
    sent = 0; worst = 0.0; t0 = time.time(); last_echo = 0.0
    while time.time() - t0 < 8:
        try: sent += flood.send(msg)
        except (BlockingIOError, InterruptedError): time.sleep(0.005)
        except OSError: break
        if time.time() - last_echo > 0.5:
            last_echo = time.time(); e0 = time.time()
            echo.sendall(frame(b"ping")); echo.recv(64); worst = max(worst, time.time() - e0)
    grow = rss_mb() - m0
    print("  ..   flood: %.1f MB of 7-byte frames sent in 8 s; server memory +%.0f MB; slowest echo %.3f s"
          % (sent / 1048576.0, grow, worst))
    if grow < 48: print("  OK   the flood did not grow the server's memory (the full inbox stopped reading)")
    else: print("  FAIL the server kept reading the flood: +%.0f MB (no backpressure)" % grow); fail = 1
    if worst < 1.0: print("  OK   other WebSocket clients still got answers")
    else: print("  FAIL another client waited %.2f s" % worst); fail = 1
    if srv.poll() is not None: print("  FAIL the server exited"); fail = 1
finally:
    srv.kill()
print("PASS: WebSocket backpressure" if not fail else "FAIL: WebSocket backpressure")
sys.exit(fail)

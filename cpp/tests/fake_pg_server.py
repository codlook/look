#!/usr/bin/env python3
# Fake PostgreSQL server — sends HOSTILE messages to LOOK's wire parser.
# Goal: on a corrupt DataRow, does LOOK silently return empty data, or does it raise an error?
import socket, struct, sys, threading, os

MODE = sys.argv[1] if len(sys.argv) > 1 else "truncated"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 55432

# commit_kopar (drop after commit) mode: write the INSERT counter to a file to share it
# across processes (a retry opens a new connection).
COUNT_FILE = os.environ.get("PG_COUNT_FILE", "/tmp/pg_insert_count")
_insert_lock = threading.Lock()
def record_insert():
    with _insert_lock:
        try: n = int(open(COUNT_FILE).read().strip())
        except Exception: n = 0
        n += 1
        open(COUNT_FILE, "w").write(str(n))
        return n

def msg(t, body):            # type + length (including itself) + body
    return t + struct.pack("!I", len(body) + 4) + body

def cstr(s):
    return s.encode() + b"\x00"

def row_desc(name="col"):
    b = struct.pack("!H", 1)                 # 1 field
    b += cstr(name)
    b += struct.pack("!IHIhih", 0, 0, 25, -1, -1, 0)   # oid=25 (text)
    return msg(b"T", b)

def data_row(mode):
    if mode == "truncated":
        # LENGTH LIES: says 100 bytes, gives 2 bytes
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 100) + b"ab")
    if mode == "negatif":
        # -5: not NULL (-1) but negative — undefined region
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", -5) + b"ab")
    if mode == "dev_uzunluk":
        # int32 max: pointer overflow attempt
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 2147483647) + b"ab")
    if mode == "eksik_alan":
        # promises 3 fields, gives 1
        return msg(b"D", struct.pack("!H", 3) + struct.pack("!i", 2) + b"ab")
    if mode == "saglam":
        return msg(b"D", struct.pack("!H", 1) + struct.pack("!i", 2) + b"ab")
    raise SystemExit("bilinmeyen mod")

def handle(c):
    # Startup packet (NO type byte): length + version + key/value
    hdr = c.recv(4)
    if len(hdr) < 4: return
    n = struct.unpack("!I", hdr)[0]
    c.recv(n - 4)
    # ── Handshake-phase stateful attacks (category 1) ──────────────────
    if MODE == "hs_error":
        # ErrorResponse instead of AuthenticationOk
        eb = b"SFATAL\x00C28000\x00Mtest reddi\x00\x00"
        c.sendall(msg(b"E", eb)); c.close(); return
    if MODE == "scram_iter":
        # SCRAM: the server asks for i=2147483647 iterations — the client must not stay in PBKDF2 for minutes
        c.sendall(msg(b"R", struct.pack("!I", 10) + cstr("SCRAM-SHA-256") + b"\x00"))
        t = c.recv(1)
        ln = struct.unpack("!I", c.recv(4))[0]
        body = b""
        while len(body) < ln - 4: body += c.recv(ln - 4 - len(body))
        first = body.split(b"n,,", 1)[-1].decode()
        cnonce = [p[2:] for p in first.split(",") if p.startswith("r=")][0]
        c.sendall(msg(b"R", struct.pack("!I", 11) + ("r=%ssrv,s=c2FsdHNhbHQ=,i=2147483647" % cnonce).encode()))
        try: c.recv(1)   # wait until the client gives up and closes
        except Exception: pass
        c.close(); return
    if MODE == "param_seli":
        c.sendall(msg(b"R", struct.pack("!I", 0)))    # AuthenticationOk
        for _ in range(200000):                        # ParameterStatus flood (memory?)
            c.sendall(msg(b"S", cstr("k") + cstr("v")))
        c.sendall(msg(b"Z", b"I"));
        # then fall into the normal query loop
    else:
        c.sendall(msg(b"R", struct.pack("!I", 0)))    # AuthenticationOk
        c.sendall(msg(b"Z", b"I"))                     # ReadyForQuery
    # Produces the reply sequence of one query (simple 'Q' OR extended P/B/D/E/S).
    # ext=True → ParseComplete('1')+BindComplete('2') are sent first (extended flow).
    # LOOK's db::query/exec/one now use NATIVE PREPARED (extended), so
    # the fake server must speak both; the bad-DataRow modes are the same in both flows.
    # Returns: True = connection closed (return), False = continue.
    def respond(q, ext):
        pre = (msg(b"1", b"") + msg(b"2", b"")) if ext else b""
        if MODE == "hs_rst":
            c.sendall(pre + row_desc() + data_row("saglam") +
                      msg(b"C", cstr("SELECT 1")) + msg(b"Z", b"I"))
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            c.close(); return True
        if MODE == "commit_kopar":
            if "INSERT" in q:
                n = record_insert()
                if n == 1:
                    c.close(); return True          # close-after-commit
                c.sendall(pre + msg(b"C", cstr("INSERT 0 1")) + msg(b"Z", b"I")); return False
            c.sendall(pre + msg(b"C", cstr("SELECT 0")) + msg(b"Z", b"I")); return False
        if MODE == "sirasiz":
            # DataRow BEFORE RowDescription (columns empty)
            c.sendall(pre + data_row("saglam") + row_desc() +
                      msg(b"C", cstr("SELECT 1")) + msg(b"Z", b"I")); return False
        if MODE == "desync":
            # FIRST: a corrupt DataRow (says 100 bytes, gives 2) + then C and Z. Later queries
            # return their own names as a single row. If the client does not close the connection on the corrupt row,
            # the unread C/Z are taken as the next query's reply: SECOND is empty, THIRD returns "second".
            if "FIRST" in q:
                c.sendall(pre + row_desc() + data_row("truncated") +
                          msg(b"C", cstr("SELECT 1")) + msg(b"Z", b"I")); return False
            name = b"second" if "SECOND" in q else b"third"
            c.sendall(pre + row_desc() + msg(b"D", struct.pack("!H", 1) + struct.pack("!i", len(name)) + name) +
                      msg(b"C", cstr("SELECT 1")) + msg(b"Z", b"I")); return False
        if MODE == "erken_c":
            c.sendall(pre + msg(b"C", cstr("SELECT 0")) + msg(b"Z", b"I")); return False
        c.sendall(pre + row_desc() + data_row(MODE) +
                  msg(b"C", cstr("SELECT 1")) + msg(b"Z", b"I")); return False

    ext_query = ""   # query text taken from Parse('P') in the extended flow
    while True:
        t = c.recv(1)
        if not t: return
        ln = struct.unpack("!I", c.recv(4))[0]
        body = b""
        need = ln - 4
        while len(body) < need:
            chunk = c.recv(need - len(body))
            if not chunk: return
            body += chunk
        if t == b"Q":                       # simple query
            q = body.decode("utf-8", "replace").upper()
            if respond(q, False): return
        elif t == b"P":                     # Parse: stmt_name\0 query\0 ...
            z1 = body.find(b"\x00")
            z2 = body.find(b"\x00", z1 + 1)
            ext_query = body[z1 + 1:z2].decode("utf-8", "replace").upper()
        elif t in (b"B", b"D"):             # Bind / Describe — reply at Sync
            pass
        elif t == b"E":                     # Execute — reply at Sync (simple model)
            pass
        elif t == b"S":                     # Sync → send the whole extended reply here
            if respond(ext_query, True): return
        elif t == b"X":
            return

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT)); s.listen(5)
print("sahte-pg hazir mod=%s port=%d" % (MODE, PORT), flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()

#!/usr/bin/env bash
# multipart/form-data: the field name is read from the name= parameter whatever the parameter
# order; an uploaded file waits in the temp directory readable only by the server's user; the
# file::store copy has the normal file mode (the web server serves it).
#
#   - `name="` was searched as text, so in `form-data; filename="a.png"; name="avatar"` (RFC 7578
#     allows any order) the field name came out as "a.png" and request::file("avatar") was null.
#   - The temp file was created with the umask's mode (0644): on a shared /tmp every local user
#     could read uploaded files. It is now 0600 and never written through an existing path.
# Usage: multipart_upload_test.sh <lk-fcgi>   (needs python3)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
PORT="${PORT:-7773}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/web" "$T/up" "$T/tmp"
cat > "$T/web/app.lk" <<'LK'
route("POST", "/up", function() {
    $f = request::file("avatar", ["allow_mime" => ["image/png"]])
    if ($f == null) { return response::text("no-file " . (request::post("title") ?? "-")) }
    $s = file::store($f, "pics")
    return response::text("file " . $f["filename"] . " title=" . (request::post("title") ?? "-") . " stored=" . $s["path"])
})
LK
export TEMP="$T/tmp" UPLOAD_DIR="$T/up"
( cd "$T/web" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.1; done

python3 - "$PORT" "$T" <<'PY'
import socket, sys, os, stat, struct, zlib
port, T = int(sys.argv[1]), sys.argv[2]; fail = 0
png = b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
png += struct.pack(">I", zlib.crc32(png[12:29]) & 0xffffffff) + b"\x00" * 16
def post(disp):
    b = "XyZb0und"
    body = (("--%s\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nhello\r\n" % b).encode() +
            ("--%s\r\nContent-Disposition: %s\r\nContent-Type: image/png\r\n\r\n" % (b, disp)).encode() + png +
            ("\r\n--%s--\r\n" % b).encode())
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5)
    s.sendall(("POST /up HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n" % (b, len(body))).encode() + body)
    out = b""
    while True:
        c = s.recv(65536)
        if not c: break
        out += c
    return out.split(b"\r\n\r\n", 1)[-1].decode("utf-8", "replace")
def check(name, ok, got):
    global fail
    print(("  OK   " if ok else "  FAIL ") + name + ": " + got)
    if not ok: fail = 1
r = post('form-data; name="avatar"; filename="a.png"')
check("name before filename", r.startswith("file a.png title=hello"), r)
r = post('form-data; filename="b.png"; name="avatar"')
check("filename before name (RFC 7578 allows any order)", r.startswith("file b.png title=hello"), r)
r = post('form-data; NAME="avatar"; filename="c\\"q.png"')
check("parameter names are case-insensitive, quoted value with \\\"", r.startswith('file c"q.png'), r)
r = post('form-data; xname="avatar"; filename="d.png"')
check("xname= is not name=", r.startswith("no-file"), r)
# geçici dosya istek bitince silinir (aşağıda istek sürerken izni yakalanır); saklanan kopya normal izinde
left = [f for f in os.listdir(os.path.join(T, "tmp"))]
check("no temp file is left behind", not left, str(left))
stored = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(T, "up")) for f in fs]
modes = sorted(set(oct(stat.S_IMODE(os.stat(p).st_mode)) for p in stored))
check("the stored copy has the normal file mode (served by the web server)", stored and all(m in ("0o644", "0o664", "0o640", "0o660") for m in modes), str(modes))
sys.exit(fail)
PY
rc=$?
# Geçici dosyanın kendisi: istek sürerken geçici dizini tarayıp iznini yakala (0600 olmalı)
cat > "$T/web/hold.lk" <<'LK'
route("POST", "/hold", function() {
    # geçici dosya gövde ayrıştırılırken (rotadan önce) yazılır; rota yalnız bekletir
    $n = 0
    for ($i = 0; $i < 3000000; $i++) { $n = $n + 1 }   # istek ~yüz ms sürsün: geçici dosya bu sırada var
    return response::text("held")
})
LK
kill $pid 2>/dev/null; wait $pid 2>/dev/null
( cd "$T/web" && exec "$FCGI" --mode http --port "$PORT" --workers 1 hold.lk > "$T/log2.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/" && break; sleep 0.1; done
( cd "$T/tmp" && while :; do for f in *; do [ -f "$f" ] && stat -c '%a' "$f" 2>/dev/null >> "$T/modes.txt"; done; done ) & w=$!
for i in $(seq 1 30); do curl -s -o /dev/null -F "avatar=@$T/web/app.lk;filename=x.txt" "localhost:$PORT/hold"; done
kill $w 2>/dev/null; wait $w 2>/dev/null
seen="$(sort -u "$T/modes.txt" 2>/dev/null | tr '\n' ' ')"
if [ -z "$seen" ]; then echo "  FAIL the temp upload file was never seen while 30 requests ran"; rc=1
elif [ "$seen" = "600 " ]; then echo "  OK   the temp upload file is 0600 while the request runs"
else echo "  FAIL the temp upload file mode: $seen"; rc=1; fi
[ $rc = 0 ] && echo "PASS: multipart parameters are parsed by name; uploads wait in a private temp file" \
           || { echo "FAIL: multipart upload"; exit 1; }

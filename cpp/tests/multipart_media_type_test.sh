#!/usr/bin/env bash
# Request bodies are read by their media type the way Go's mime.ParseMediaType / mime/multipart
# read them:
#   * boundary="B" (quoted — .NET MultipartFormDataContent and some Java clients send it),
#     "boundary=B; charset=utf-8" and "BOUNDARY=B" are read — every field used to be silently empty;
#   * a part header written "content-disposition:" is read (header names are case-insensitive);
#   * more than 1000 parts is a 413, a missing or invalid boundary a 400 — the extra parts used to be
#     dropped silently;
#   * urlencoded bodies with a charset parameter, or an upper-case media type, still work.
# Usage: multipart_media_type_test.sh <lk-fcgi> [port]   (needs python3)
set -u
FCGI="$(cd "$(dirname "${1:?usage: multipart_media_type_test.sh <lk-fcgi> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7941}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("POST", "/f", function() { $f = request::file("doc"); return response::text("a=" . (request::post("a") ?? "-") . " file=" . ($f == null ? "-" : $f["filename"])) })
LK
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 1 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), timeout=1).close()" 2>/dev/null && break; sleep 0.1; done
python3 - "$PORT" <<'PY'
import socket, sys
P = int(sys.argv[1]); fail = 0
def post(ct, body):
    req = (b"POST /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Type: " + ct +
           b"\r\nContent-Length: %d\r\n\r\n" % len(body)) + body
    s = socket.create_connection(("127.0.0.1", P)); s.sendall(req); out = b""
    while True:
        c = s.recv(65536)
        if not c: break
        out += c
    head, _, b = out.partition(b"\r\n\r\n")
    return head.split(b" ")[1].decode(), b.decode(errors="replace")
def part(name, val, hdr=b"Content-Disposition", fn=None):
    d = hdr + b': form-data; name="' + name + b'"' + ((b'; filename="' + fn + b'"') if fn else b"")
    return b"--B\r\n" + d + b"\r\n\r\n" + val + b"\r\n"
base = part(b"a", b"1") + part(b"doc", b"hello", fn=b"x.txt") + b"--B--\r\n"
rows = [
    ("boundary=B",                     b"multipart/form-data; boundary=B", base, "200", "a=1 file=x.txt"),
    ('quoted boundary="B"',            b'multipart/form-data; boundary="B"', base, "200", "a=1 file=x.txt"),
    ("boundary=B; charset=utf-8",      b"multipart/form-data; boundary=B; charset=utf-8", base, "200", "a=1 file=x.txt"),
    ("BOUNDARY=B",                     b"multipart/form-data; BOUNDARY=B", base, "200", "a=1 file=x.txt"),
    ("Multipart/Form-Data (case)",     b"Multipart/Form-Data; boundary=B", base, "200", "a=1 file=x.txt"),
    ("content-disposition lower case", b"multipart/form-data; boundary=B", part(b"a", b"1", hdr=b"content-disposition") + b"--B--\r\n", "200", "a=1 file=-"),
    ("1001 parts",                     b"multipart/form-data; boundary=B", b"".join(part(b"f%d" % i, b"v") for i in range(1001)) + b"--B--\r\n", "413", "more than 1000 parts"),
    ("1000 parts",                     b"multipart/form-data; boundary=B", part(b"a", b"1") + b"".join(part(b"f%d" % i, b"v") for i in range(999)) + b"--B--\r\n", "200", "a=1"),
    ("empty boundary",                 b"multipart/form-data; boundary=", base, "400", "valid boundary"),
    ("no boundary",                    b"multipart/form-data", base, "400", "valid boundary"),
    ("boundary over 70 characters",    b"multipart/form-data; boundary=" + b"x" * 71, base, "400", "valid boundary"),
    ("urlencoded",                     b"application/x-www-form-urlencoded", b"a=1", "200", "a=1 file=-"),
    ("urlencoded; charset=UTF-8",      b"application/x-www-form-urlencoded; charset=UTF-8", b"a=1", "200", "a=1 file=-"),
    ("urlencoded, upper case type",    b"Application/X-WWW-Form-Urlencoded", b"a=1", "200", "a=1 file=-"),
    ("another type is not a form",     b"text/plain; note=application/x-www-form-urlencoded", b"a=1", "200", "a=- file=-"),
]
for name, ct, body, wc, wb in rows:
    code, b = post(ct, body)
    ok = code == wc and wb in b
    print("  %-4s %-32s %s %s" % ("OK" if ok else "FAIL", name, code, b[:60]))
    fail |= not ok
sys.exit(fail)
PY
rc=$?
[ $rc = 0 ] && echo "PASS: request bodies are read by their media type (Go mime model)" || { echo "FAIL: media type parsing"; exit 1; }

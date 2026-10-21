#!/usr/bin/env bash
# httpoxy (CVE-2016-5385 class): under CGI a client's "Proxy:" request header becomes the HTTP_PROXY
# environment variable. An HTTP client that reads HTTP_PROXY would send the application's outgoing
# requests (payment APIs, OAuth calls) through the attacker's proxy. LOOK's http:: client reads no
# proxy variable; this test keeps it that way.
#   * lk-cgi with HTTP_PROXY / http_proxy set to a listening "proxy": http::get goes to its target,
#     the proxy receives nothing
#   * positive control: a request sent to the proxy port on purpose is recorded — the detector works
# Usage: httpoxy_test.sh <lk-cgi>
CGI="$(cd "$(dirname "${1:?usage: $0 <lk-cgi>}")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"; P1=""; P2=""
trap 'rm -rf "$TMP"; [ -n "$P1" ] && kill $P1 2>/dev/null; [ -n "$P2" ] && kill $P2 2>/dev/null' EXIT
PPORT="${PPORT:-18080}"; TPORT="${TPORT:-18081}"
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
cat > "$TMP/listen.py" <<'PY'
import socket, sys
port, name, log = int(sys.argv[1]), sys.argv[2], sys.argv[3]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(16)
while True:
    c, _ = s.accept()
    d = c.recv(4096)
    with open(log, "a") as f: f.write(name + " " + d.split(b"\r\n")[0].decode(errors="replace") + "\n")
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" + name.encode()[:6].ljust(6))
    c.close()
PY
python3 "$TMP/listen.py" "$PPORT" proxy "$TMP/hits.log" & P1=$!
python3 "$TMP/listen.py" "$TPORT" target "$TMP/hits.log" & P2=$!
sleep 0.5
mkapp() { cat > "$TMP/app.lk" <<LK
use http
route("GET", "/", function() {
    \$r = http::get("http://127.0.0.1:$1/data")
    \$b = \$r["body"]
    return response::text("body=[{\$b}]")
})
LK
}
call() {   # call <proxy var> → $out
    out="$(cd "$TMP" && env -i PATH="$PATH" MSYS2_ENV_CONV_EXCL="PATH_INFO;REQUEST_URI" REQUEST_METHOD=GET \
        SCRIPT_FILENAME="$TMP/app.lk" PATH_INFO=/ REQUEST_URI=/ GATEWAY_INTERFACE=CGI/1.1 LOOK_ALLOW_SSRF=1 \
        "$1=http://127.0.0.1:$PPORT" "$CGI" 2>&1 | tr -d '\r' | tail -1)"
}
fail=0
mkapp "$TPORT"
for var in HTTP_PROXY http_proxy; do
    rm -f "$TMP/hits.log"; call "$var"
    if [ "$out" = "body=[target]" ] && ! grep -q '^proxy' "$TMP/hits.log" 2>/dev/null; then
        echo "  OK   $var set by a request header: http::get went to its target, the proxy got nothing"
    else echo "  FAIL $var: response=[$out] connections=[$(tr '\n' ';' < "$TMP/hits.log" 2>/dev/null)]"; fail=1; fi
done
# positive control: the detector sees a request that does reach the proxy port
mkapp "$PPORT"; rm -f "$TMP/hits.log"; call HTTP_PROXY
if grep -q '^proxy GET /data' "$TMP/hits.log" 2>/dev/null; then echo "  OK   positive control: a request to the proxy port is recorded"
else echo "  FAIL positive control: the proxy listener recorded nothing [$out]"; fail=1; fi
[ $fail = 0 ] && echo "PASS: no httpoxy — a Proxy: header does not redirect outgoing requests" || { echo "FAIL: httpoxy"; exit 1; }

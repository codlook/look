#!/usr/bin/env bash
# http:: URL handling (Go net/url model), against an echo server that answers with the request
# line, Host and Authorization it received:
#   * the authority ends at '/', '?' or '#' — "http://h:p?q=1" used to put "?q=1" into the host;
#   * the fragment is not sent; the scheme is case-insensitive;
#   * "user:pass@" becomes Basic credentials (percent-decoded) — it used to end up in the host;
#     an Authorization header given by the caller wins;
#   * a bad port is an error that names it — "http://h:80x/" used to connect to port 80;
#   * an IPv6 address in brackets connects and is sent as "Host: [::1]:port";
#   * an https:// proxy is refused (LOOK talks plain HTTP to a proxy) instead of silently used.
# Usage: http_client_url_test.sh <lk> [port]
set -u
LK="$(cd "$(dirname "${1:?usage: http_client_url_test.sh <lk> [port]}")" && pwd)/$(basename "$1")"
PORT="${2:-7797}"
T="$(mktemp -d)"; up=""; trap '[ -n "$up" ] && kill $up 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/echo.py" <<'PY'
import socket, sys, threading
s = socket.socket(socket.AF_INET6); s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("::", int(sys.argv[1]))); s.listen(16)
def h(c):
    d = b""
    while b"\r\n\r\n" not in d:
        x = c.recv(4096)
        if not x: break
        d += x
    lines = d.split(b"\r\n")
    keep = [lines[0]] + [l for l in lines[1:] if l.lower().startswith((b"host:", b"authorization:", b"user-agent:"))]
    body = b"\n".join(keep)
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body); c.close()
print("ready", flush=True)
while True:
    c, _ = s.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
PY
python3 "$T/echo.py" "$PORT" > "$T/echo.log" 2>&1 & up=$!
for i in $(seq 1 50); do grep -q ready "$T/echo.log" && break; sleep 0.1; done
v6=1; python3 -c "import socket; socket.create_connection(('::1', $PORT), timeout=2).close()" 2>/dev/null || v6=0

fail=0
run() {   # ad url beklenen-metin [ek seçenek/başlık lk ifadesi]
    cat > "$T/q.lk" <<LK
use http
\$r = http::get("$2"${4:-})
print((\$r["body"] ?? "") . "|ERR=" . (\$r["error"] ?? ""))
LK
    out="$(cd "$T" && LOOK_ALLOW_SSRF=1 "$LK" q.lk 2>&1 | grep -v '^\[20' | tr '\n' ' ')"
    if echo "$out" | grep -qF -- "$3"; then echo "  OK   $1"; else echo "  FAIL $1: want \"$3\", got: $out"; fail=1; fi
}
B="http://127.0.0.1:$PORT"
run "path and query"           "$B/a?x=1"                "GET /a?x=1 HTTP/1.1"
run "query without a path"     "$B?x=1"                  "GET /?x=1 HTTP/1.1"
run "query-only host clean"    "$B?x=1"                  "Host: 127.0.0.1:$PORT"
run "fragment not sent"        "$B/p#frag"               "GET /p HTTP/1.1"
run "fragment without a path"  "$B#frag"                 "GET / HTTP/1.1"
run "scheme in capitals"       "HTTP://127.0.0.1:$PORT/c" "GET /c HTTP/1.1"
run "userinfo -> Basic"        "http://al%40x:p%3Aw@127.0.0.1:$PORT/u" "Authorization: Basic YWxAeDpwOnc="
run "userinfo host clean"      "http://al:pw@127.0.0.1:$PORT/u" "Host: 127.0.0.1:$PORT"
run "caller Authorization wins" "http://al:pw@127.0.0.1:$PORT/u" "Authorization: Bearer t" ', ["Authorization" => "Bearer t"]'
run "bad port 80x"             "http://127.0.0.1:${PORT}x/" 'ERR=http:: invalid URL: invalid port'
run "port 99999"               "http://127.0.0.1:99999/" 'ERR=http:: invalid URL: invalid port "99999"'
run "IPv6 without brackets"    "http://::1:$PORT/"       'must be in brackets'
run "https proxy refused"      "$B/p"                    'invalid proxy "https://127.0.0.1:1"' ', [], ["proxy" => "https://127.0.0.1:1"]'
run "user agent is the version" "$B/ua"                  "User-Agent: LOOK/1."
if [ $v6 = 1 ]; then
    run "IPv6 in brackets"     "http://[::1]:$PORT/six"  "GET /six HTTP/1.1"
    run "IPv6 Host header"     "http://[::1]:$PORT/six"  "Host: [::1]:$PORT"
elif [ -n "${CI:-}" ]; then echo "  FAIL ::1 is not reachable"; fail=1
else echo "  SKIP IPv6 rows: ::1 is not reachable"; fi
[ $fail = 0 ] && echo "PASS: http:: URL handling" || { echo "FAIL: http:: URL handling"; exit 1; }

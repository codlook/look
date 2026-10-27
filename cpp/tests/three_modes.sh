# three_modes.sh — the same request to --mode http, FastCGI and CGI (sourced by tests).
#
# Session and cookie security was tested in --mode http only, and CGI fell behind three times:
# lk-cgi was in no package, its expired sessions were never removed, and it never read HTTPS
# (nor any request header). A test that sources this file runs each check in all three modes, so
# a behaviour one mode lacks turns CI red.
#
#   source three_modes.sh <lk-fcgi> <lk-cgi> <app.lk>
#   tm_start [ENV=value ...]           start --mode http and FastCGI (and remember env for CGI)
#   tm_req <mode> <method> <path> [options]   → prints the status; headers in $TM/h, body in $TM/b
#       -c <cookie header value>   -H "<Name>: <value>"   -d <urlencoded body>
#       --https          the connection is HTTPS, signalled the way each mode gets it:
#                        http: X-Forwarded-Proto from a trusted proxy (tm_start LOOK_TRUSTED_PROXY=127.0.0.1)
#                        fcgi: the HTTPS=on parameter;  cgi: the HTTPS=on variable
#   tm_cookie <name>  → the value set by the last response (name=value), tm_cookie_line <name> → the line
#   tm_stop
TM_LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
TM_CGI="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
TM_APP="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
TM="$(mktemp -d)"; mkdir -m 700 "$TM/s"
TM_P=${TM_P:-7681}; TM_FP=${TM_FP:-7682}
TM_MODES="http fcgi cgi"
tm_hpid=""; tm_fpid=""; TM_ENV=()
cat > "$TM/fcgi.py" <<'EOF'
import os, socket, struct, sys
def rec(t, c): return struct.pack(">BBHHBx", 1, t, 1, len(c), 0) + c
def ln(n): return bytes([n]) if n < 128 else struct.pack(">I", n | 0x80000000)
def nv(k, v): k, v = k.encode(), v.encode(); return ln(len(k)) + ln(len(v)) + k + v
p = dict(a.split("=", 1) for a in sys.argv[2:])
body = os.environ.get("TM_BODY", "").encode()
if body: p["CONTENT_LENGTH"] = str(len(body))
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(rec(1, struct.pack(">HB5x", 1, 0)) + rec(4, b"".join(nv(k, v) for k, v in p.items())) + rec(4, b"")
          + (rec(5, body) if body else b"") + rec(5, b""))
buf, res = b"", b""
while True:
    d = s.recv(65536)
    if not d: break
    buf += d
    while len(buf) >= 8:
        _, t, _, cl, pl = struct.unpack(">BBHHB", buf[:7])
        if len(buf) < 8 + cl + pl: break
        if t == 6: res += buf[8:8 + cl]
        buf = buf[8 + cl + pl:]
        if t == 3: sys.stdout.buffer.write(res); sys.exit(0)
EOF

tm_stop() { { kill $tm_hpid $tm_fpid; wait $tm_hpid $tm_fpid; } 2>/dev/null; tm_hpid=""; tm_fpid=""; }
tm_start() {
    tm_stop; TM_ENV=("$@")
    ( cd "$TM" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET="${LOOK_SECRET:-}" LOOK_SESSION_DIR="$TM/s" "${TM_ENV[@]}" \
        "$TM_LKF" --mode http --port $TM_P --workers 2 "$TM_APP" >"$TM/hlog" 2>&1 ) & tm_hpid=$!
    ( cd "$TM" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET="${LOOK_SECRET:-}" LOOK_SESSION_DIR="$TM/s" "${TM_ENV[@]}" \
        "$TM_LKF" --port $TM_FP --workers 2 --script "$TM_APP" >"$TM/flog" 2>&1 ) & tm_fpid=$!
    local i
    for i in $(seq 1 50); do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$TM_P/" && \
        python3 "$TM/fcgi.py" $TM_FP REQUEST_METHOD=GET REQUEST_URI=/ SCRIPT_FILENAME="$TM_APP" >/dev/null 2>&1 && return 0
        { kill -0 $tm_hpid && kill -0 $tm_fpid; } 2>/dev/null || { echo "tm_start: a server did not start"; tail -n 3 "$TM/hlog" "$TM/flog"; return 1; }
        sleep 0.2
    done
    return 1
}
tm_split() {   # $TM/raw (CGI/FastCGI output) → $TM/h, $TM/b; prints the status
    tr -d '\r' < "$TM/raw" | sed '/^$/q' > "$TM/h"
    tr -d '\r' < "$TM/raw" | sed '1,/^$/d' > "$TM/b"
    local st; st=$(grep -i '^status:' "$TM/h" | head -1 | awk '{print $2}'); echo "${st:-200}"
}
tm_req() {
    local mode="$1" m="$2" path="$3"; shift 3
    local ck="" body="" https=0 hdrs=()
    while [ $# -gt 0 ]; do case "$1" in
        -c) ck="$2"; shift 2 ;; -H) hdrs+=("$2"); shift 2 ;; -d) body="$2"; shift 2 ;;
        --https) https=1; shift ;; *) echo "tm_req: unknown option $1" >&2; return 2 ;; esac; done
    local h name val var
    case "$mode" in
    http)
        local a=(-s -D "$TM/h" -o "$TM/b" --max-time 5 -X "$m")
        [ -n "$ck" ] && a+=(-H "Cookie: $ck"); [ $https = 1 ] && a+=(-H "X-Forwarded-Proto: https")
        for h in "${hdrs[@]}"; do a+=(-H "$h"); done
        [ -n "$body" ] && a+=(-H "Content-Type: application/x-www-form-urlencoded" --data-binary "$body")
        curl "${a[@]}" "http://127.0.0.1:$TM_P$path" >/dev/null; tr -d '\r' < "$TM/h" > "$TM/h2"; mv "$TM/h2" "$TM/h"
        head -1 "$TM/h" | awk '{print $2}' ;;
    fcgi|cgi)
        local kv=(REQUEST_METHOD="$m" REQUEST_URI="$path" SCRIPT_FILENAME="$TM_APP" HTTP_HOST="127.0.0.1")
        [ -n "$ck" ] && kv+=(HTTP_COOKIE="$ck"); [ $https = 1 ] && kv+=(HTTPS=on)
        [ -n "$body" ] && kv+=(CONTENT_TYPE=application/x-www-form-urlencoded)
        for h in "${hdrs[@]}"; do
            name="${h%%:*}"; val="${h#*:}"; val="${val# }"
            var="HTTP_$(echo "$name" | tr 'a-z-' 'A-Z_')"; kv+=("$var=$val")
        done
        if [ "$mode" = fcgi ]; then
            TM_BODY="$body" python3 "$TM/fcgi.py" $TM_FP "${kv[@]}" > "$TM/raw" 2>/dev/null
        else
            [ -n "$body" ] && kv+=(CONTENT_LENGTH="${#body}")
            (cd "$TM" && printf '%s' "$body" | env -i PATH=/usr/bin:/bin LOOK_SECRET="${LOOK_SECRET:-}" LOOK_SESSION_DIR="$TM/s" "${TM_ENV[@]}" \
                PATH_INFO="$path" GATEWAY_INTERFACE=CGI/1.1 "${kv[@]}" "$TM_CGI" > "$TM/raw" 2>>"$TM/clog")
        fi
        tm_split ;;
    esac
}
tm_cookie()      { grep -i "^set-cookie: $1=" "$TM/h" | tail -1 | sed 's/^[^:]*: //; s/;.*//'; }
tm_cookie_line() { grep -i "^set-cookie: $1=" "$TM/h" | tail -1; }
tm_log()         { cat "$TM/hlog" "$TM/flog" "$TM/clog" 2>/dev/null; }
tm_cleanup()     { tm_stop; rm -rf "$TM"; }

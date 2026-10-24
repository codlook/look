#!/usr/bin/env bash
# Redis sessions never reach another user's request.
#
# LOOK 1 keeps one Redis connection per worker thread, used by every request on that thread in
# turn. If a reply is left half-read on a connection that stays open, the next request on that
# thread reads the rest of it as its own reply, and from then on every request reads the reply
# meant for the request before it — one user's session data goes to another user. A reply is
# left half-read when the client gives up in the middle of it: a session bigger than
# LOOK_REDIS_MAX_BULK. The connection is now dropped on every error (and a Redis restart can no
# longer leave a thread with a dead connection).
#   1. Redis restarted while users A and B are logged in: neither ever reads the other's data.
#   2. A's session grows past LOOK_REDIS_MAX_BULK: B and C, on the same worker thread, must each
#      read only their own session afterwards.
# Usage: redis_session_crossing_test.sh <lk-fcgi>   (needs python3, curl)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap '{ kill $pid $rpid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""; rpid=""
cat > "$T/fake_redis.py" <<'PY'
import socket, sys, threading
store = {}
def rd(f):
    line = f.readline()
    if not line: return None
    n = int(line[1:]); args = []
    for _ in range(n):
        ln = int(f.readline()[1:]); args.append(f.read(ln + 2)[:-2])
    return args
def h(c):
    f = c.makefile("rb")
    while True:
        a = rd(f)
        if a is None: break
        cmd = a[0].upper()
        if cmd == b"GET":
            v = store.get(a[1]); c.sendall(b"$-1\r\n" if v is None else b"$%d\r\n%s\r\n" % (len(v), v))
        elif cmd == b"SET": store[a[1]] = a[2]; c.sendall(b"+OK\r\n")
        elif cmd == b"DEL": store.pop(a[1], None); c.sendall(b":1\r\n")
        else: c.sendall(b"+OK\r\n")
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(16)
while True:
    c, _ = s.accept(); threading.Thread(target=h, args=(c,), daemon=True).start()
PY
cat > "$T/app.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", request::get("v")); return response::text("ok") })
route("GET", "/get", function() { session::start(); return response::text("v=" . session::get("v")) })
LK
RP=7496; P=7497
start_redis() { python3 "$T/fake_redis.py" $RP & rpid=$!; sleep 0.3; }
start_lk() {   # $1 = ek ortam (ör. LOOK_REDIS_MAX_BULK=...)
    env $1 LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="redis://127.0.0.1:$RP" LOOK_SESSION_SECURE=0 \
        "$LKF" --mode http --port $P --workers 1 "$T/app.lk" >"$T/srv.log" 2>&1 & pid=$!
    for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P/health" >/dev/null && break; sleep 0.2; done
}
q() { curl -s -b "$T/cj_$1" -c "$T/cj_$1" --max-time 5 "http://127.0.0.1:$P$2"; }   # q <kullanıcı> <yol>
fail=0
# 1. Redis yeniden başlatma
start_redis; start_lk ""
q A "/set?v=A0" >/dev/null; q B "/set?v=B0" >/dev/null
kill $rpid; wait $rpid 2>/dev/null; start_redis
crossed=""; reads=""
for i in 1 2 3 4 5 6; do
    q A "/set?v=A$i" >/dev/null; q B "/set?v=B$i" >/dev/null
    a=$(q A /get); b=$(q B /get); reads="$reads $a/$b"
    case "$a" in *B*) crossed="$crossed A-read-$a";; esac
    case "$b" in *A*) crossed="$crossed B-read-$b";; esac
done
if [ -z "$crossed" ]; then echo "PASS: after a Redis restart no user read another user's session (A/B reads:$reads)"
else echo "FAIL: after a Redis restart sessions crossed:$crossed (A/B reads:$reads)"; fail=1; fi
{ kill $pid $rpid; wait $pid $rpid; } 2>/dev/null
rm -f "$T"/cj_*
# 2. Yarım okunmuş yanıt (oturum LOOK_REDIS_MAX_BULK'tan büyük)
start_redis; start_lk "LOOK_REDIS_MAX_BULK=300"
q B "/set?v=B-secret" >/dev/null; q C "/set?v=C-secret" >/dev/null
big=$(head -c 400 /dev/zero | tr '\0' 'x')
q A "/set?v=$big" >/dev/null; q A /get >/dev/null      # A'nın oturumu sınırın üstünde
crossed=""; reads=""
for i in 1 2 3 4; do
    b=$(q B /get); c=$(q C /get); reads="$reads $b/$c"
    case "$b" in *C-secret*|*xxxx*) crossed="$crossed B-read-other";; esac
    case "$c" in *B-secret*|*xxxx*) crossed="$crossed C-read-other";; esac
done
if [ -z "$crossed" ]; then echo "PASS: after an over-limit session reply each user read only their own session (B/C reads:$reads)"
else echo "FAIL: after an over-limit session reply sessions crossed:$crossed (B/C reads:$(echo "$reads" | cut -c1-200))"; fail=1; fi
exit $fail

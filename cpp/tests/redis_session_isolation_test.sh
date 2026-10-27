#!/usr/bin/env bash
# Redis sessions: each request sees only its own session, and a Redis that stops answering
# gives the request a clear error instead of hanging it.
#
# Found in v2 (never released): session storage kept ONE Redis connection per worker thread.
# Once a Redis wait suspended only the fiber (not the thread), two requests on the same thread
# could share that connection — one request's GET reply could be read by another request, i.e.
# user B received user A's session. This test runs one worker (all requests share a thread),
# gives every client its own session value, and checks each client only ever reads its own.
# Then it points the server at a Redis that accepts but never answers (a silent TCP listener)
# and checks the request gets 503 within LOOK_REDIS_TIMEOUT_MS (logged) instead of hanging; an
# application try/catch sees the error; with no Redis at all the request gets 503, not an empty
# session (LOOK 1 swallowed Redis errors: users looked logged out and session data was lost).
# Usage: redis_session_isolation_test.sh <lk-fcgi> <redis-url>   (needs python3, curl)
set -u
# LOOK 2: çerez kullanan uygulama LOOK_SECRET olmadan başlamaz (CSRF imzası) — testin kendi anahtarı.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$1"; URL="$2"
PORT=7461; SPORT=7462; MUTE=7463; DPORT=7464; DOWN=7465
T=$(mktemp -d); trap '{ kill $pid $spid $mpid $dpid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""; spid=""; mpid=""; dpid=""
cat > "$T/app.lk" <<'LK'
route("GET", "/set", function() { session::start(); session::set("v", request::get("v")); return response::text("ok") })
route("GET", "/get", function() { session::start(); return response::text(session::get("v")) })
route("GET", "/ping", function() { return response::text("ok") })
route("GET", "/caught", function() { try { session::start(); session::set("a", "b") } catch ($e) { return response::text("caught: " . $e) } return response::text("no error") })
LK
wait_up() { for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$1/health" | grep -q ok && return 0; sleep 0.2; done; return 1; }
fail=0

LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="$URL" "$LKF" --mode http --port $PORT --workers 1 "$T/app.lk" >"$T/srv.log" 2>&1 &
pid=$!
wait_up $PORT || { echo "FAIL: server did not start"; cat "$T/srv.log"; exit 1; }

python3 - "$PORT" <<'PY' || fail=1
import http.client, sys, threading
port = int(sys.argv[1]); N, ROUNDS = 64, 40
bad = []; errs = []
def client(i):
    try:
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        c.request("GET", "/set?v=user%d" % i); r = c.getresponse(); r.read()
        cookie = r.getheader("Set-Cookie", "").split(";")[0]
        for _ in range(ROUNDS):
            c.request("GET", "/get", headers={"Cookie": cookie}); r = c.getresponse()
            got = r.read().decode()
            if got != "user%d" % i: bad.append((i, got)); return
    except Exception as e:
        errs.append((i, repr(e)))
ts = [threading.Thread(target=client, args=(i,)) for i in range(N)]
[t.start() for t in ts]; [t.join() for t in ts]
if bad or errs:
    print("FAIL: sessions crossed between requests: %d wrong reads, %d errors; e.g. %s %s"
          % (len(bad), len(errs), bad[:3], errs[:3]))
    sys.exit(1)
print("PASS: %d concurrent clients x %d reads on one worker, each saw only its own session" % (N, ROUNDS))
PY

# Redis kabul eden ama hiç yanıt vermeyen sunucu: istek süre sınırında açık hatayla dönmeli.
python3 -c "
import socket, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $MUTE)); s.listen(64); keep = []
while True: keep.append(s.accept()[0])
" & mpid=$!
sleep 0.3
LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="redis://127.0.0.1:$MUTE" LOOK_REDIS_TIMEOUT_MS=500 \
    "$LKF" --mode http --port $SPORT --workers 1 "$T/app.lk" >"$T/mute.log" 2>&1 &
spid=$!
wait_up $SPORT || { echo "FAIL: server (silent Redis) did not start"; cat "$T/mute.log"; exit 1; }
s0=$(date +%s%N)
code=$(curl -s -o "$T/body" -w '%{http_code}' --max-time 10 "http://127.0.0.1:$SPORT/set?v=x")
ms=$(( ($(date +%s%N) - s0) / 1000000 ))
# Bekleme sırasında aynı işçi başka isteğe bakabilmeli (yalnız fiber bekliyor).
if [ "$code" = "503" ] && [ "$ms" -lt 3000 ] && grep -q "LOOK_REDIS_TIMEOUT_MS" "$T/mute.log" \
   && grep -q "temporarily unavailable" "$T/body"; then
    echo "PASS: a silent Redis gives 503 after ${ms} ms; the log names LOOK_REDIS_TIMEOUT_MS"
else
    echo "FAIL: silent Redis: status=$code after ${ms} ms, body '$(head -c 80 "$T/body")' (want 503 within the 500 ms limit, log naming LOOK_REDIS_TIMEOUT_MS)"
    tail -5 "$T/mute.log"; fail=1
fi
h=$(curl -s --max-time 2 "http://127.0.0.1:$SPORT/ping")
[ "$h" = "ok" ] && echo "PASS: the server still answers after the Redis timeout" \
                || { echo "FAIL: server stopped answering after the Redis timeout"; fail=1; }
# Uygulama hatayı kendisi yakalayabilir: try/catch sıradan bir hata mesajı görür.
c=$(curl -s --max-time 10 "http://127.0.0.1:$SPORT/caught")
case "$c" in "caught: session: Redis:"*) echo "PASS: try/catch in the application sees the Redis error";;
             *) echo "FAIL: try/catch did not see the Redis error: '$c'"; fail=1;; esac
# Redis hiç yok (bağlantı reddi): eskiden oturum sessizce boştu, yazılan veri kayboluyordu.
LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="redis://127.0.0.1:$DOWN" \
    "$LKF" --mode http --port $DPORT --workers 1 "$T/app.lk" >"$T/down.log" 2>&1 &
dpid=$!
wait_up $DPORT || { echo "FAIL: server (no Redis) did not start"; cat "$T/down.log"; exit 1; }
code=$(curl -s -o "$T/body2" -w '%{http_code}' --max-time 10 "http://127.0.0.1:$DPORT/set?v=x")
if [ "$code" = "503" ] && grep -q "session: Redis: cannot connect" "$T/down.log"; then
    echo "PASS: no Redis at all gives 503 and a logged error (not an empty session)"
else
    echo "FAIL: no Redis: status=$code (want 503 + logged 'cannot connect')"; tail -3 "$T/down.log"; fail=1
fi
exit $fail

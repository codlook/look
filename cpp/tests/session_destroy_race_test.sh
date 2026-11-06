#!/usr/bin/env bash
# session::destroy always wins over a session::set running at the same time: after a logout the
# old session id is not valid again, whichever request finishes last.
#
# session::set checked that the session existed, read it, and wrote it back. A logout (another
# tab, another request of the same browser) landing between the check and the write was undone:
# the write created the session file again under the old id, with the data read before the
# logout. Now a write replaces the session only while it still exists (file driver: lock + link
# check; Redis: SET ... XX), and the removal takes the same lock.
# Usage: session_destroy_race_test.sh <lk-fcgi> [redis-url]   (needs python3)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
FCGI="$(cd "$(dirname "${1:?usage}")" && pwd)/$(basename "$1")"
RURL="${2:-}"
PORT="${PORT:-7774}"
T="$(mktemp -d)"; pid=""; trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
route("GET", "/login", function() { session::start(); session::regenerate(); session::set("user", "victim"); return response::text(session::start()) })
route("GET", "/touch", function() { session::set("seen", request::get("n", "0")); return response::text("ok") })
route("GET", "/logout", function() { session::destroy(); return response::text("bye") })
route("GET", "/who", fn() => response::text(session::get("user") ?? "none"))
LK
if [ -n "$RURL" ]; then export LOOK_SESSION_DRIVER=redis LOOK_REDIS_URL="$RURL"; drv=redis; else export LOOK_SESSION_DIR="$T/s"; drv=file; fi
( cd "$T" && exec "$FCGI" --mode http --port "$PORT" --workers 8 app.lk > "$T/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/who" && break; sleep 0.1; done

python3 - "$PORT" <<'PY'
import http.client, threading, sys, re
port = int(sys.argv[1])
def get(path, sid=None):
    h = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    h.request("GET", path, headers={"Cookie": "LOOK_SESSION=" + sid} if sid else {})
    r = h.getresponse(); body = r.read().decode()
    h.close(); return body
rounds, back = 150, 0
for k in range(rounds):
    sid = get("/login")
    bar = threading.Barrier(21)
    def touch(i):
        bar.wait(); get("/touch?n=%d" % i, sid)
    def logout():
        bar.wait(); get("/logout", sid)
    ts = [threading.Thread(target=touch, args=(i,)) for i in range(20)] + [threading.Thread(target=logout)]
    [t.start() for t in ts]; [t.join() for t in ts]
    if get("/who", sid) != "none": back += 1
print("rounds=%d resurrected=%d" % (rounds, back))
sys.exit(1 if back else 0)
PY
rc=$?
kill -0 "$pid" 2>/dev/null || { echo "  FAIL the server died"; rc=1; }
[ $rc = 0 ] && echo "PASS [$drv]: a logout is never undone by a session::set running at the same time" \
           || { echo "FAIL [$drv]: a logout was undone by a concurrent session::set"; exit 1; }

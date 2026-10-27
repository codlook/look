#!/usr/bin/env bash
# Expired session files are removed, and an expired session is not a session.
#
# The file driver never removed a session file: every request that writes to the session of a
# new visitor (a CSRF token, as LookPress does on every page) left a file behind for ever.
# Requests without a cookie could fill the disk and its inodes without logging in — on a
# shared /tmp, the whole server. And LOOK_SESSION_TTL was not applied to files at all: an old
# session file was still a valid session. Now a file not written for LOOK_SESSION_TTL seconds
# is not read, and expired files are swept in the background (at most every 5 minutes, or
# every TTL seconds when the TTL is shorter).
# Usage: session_gc_test.sh <lk-fcgi>   (needs curl)
set -u
# LOOK 2: çerez kullanan uygulama LOOK_SECRET olmadan başlamaz (CSRF imzası) — testin kendi anahtarı.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7620
T=$(mktemp -d); trap '{ kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""
mkdir -m 0700 "$T/s"
cat > "$T/app.lk" <<'LK'
route("GET", "/csrf", function() { session::start(); session::set("csrf", "token"); return response::text("ok") })
route("GET", "/login", function() { session::start(); session::set("user", "alice"); return response::text("ok") })
route("GET", "/who", function() { session::start(); return response::text("user=" . session::get("user")) })
LK
( cd "$T" && exec env LOOK_SESSION_DIR="$T/s" TMPDIR="$T/s" TEMP="$T/s" LOOK_SESSION_TTL=2 LOOK_SESSION_SECURE=0 \
    "$LKF" --mode http --port $P --workers 2 app.lk >"$T/srv.log" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P/health" >/dev/null && break; sleep 0.2; done
files() { find "$T/s" -name 'look_sess_*' 2>/dev/null | wc -l; }
fail=0
for i in $(seq 1 300); do curl -s -o /dev/null --max-time 5 "http://127.0.0.1:$P/csrf"; done
before=$(files)
curl -s -c "$T/cj" -o /dev/null --max-time 5 "http://127.0.0.1:$P/login"
now=$(curl -s -b "$T/cj" --max-time 5 "http://127.0.0.1:$P/who")
[ "$now" = "user=alice" ] && echo "  OK   a fresh session is read ($now)" || { echo "  FAIL a fresh session: '$now'"; fail=1; }
sleep 4                                                              # TTL 2 s doldu
curl -s -o /dev/null --max-time 5 "http://127.0.0.1:$P/csrf"         # temizliği tetikler
sleep 1
after=$(files)
if [ "$after" -le 3 ]; then echo "  OK   expired files removed: $before files after 300 requests without a cookie, $after after the TTL"
else echo "  FAIL expired files stay: $before files after 300 requests without a cookie, still $after after the TTL"; fail=1; fi
old=$(curl -s -b "$T/cj" --max-time 5 "http://127.0.0.1:$P/who")
[ "$old" = "user=alice" ] && { echo "  FAIL an expired session is still read ($old)"; fail=1; } \
                         || echo "  OK   an expired session is not read ($old)"
[ $fail = 0 ] && echo "PASS: expired sessions are not read and their files are removed" || echo "FAIL: session expiry"
exit $fail

#!/usr/bin/env bash
# --bind / LOOK_BIND: --mode http listens only on the address given.
#
# lk-fcgi --mode http always listened on every interface. Behind a reverse proxy (Plesk: Apache
# → 127.0.0.1:<port>) the site was also open on its own port to the internet: the proxy could be
# bypassed, and with it HTTPS, the proxy's rules and the trust in its X-Forwarded-* headers
# (measured on a Plesk server: http://<public ip>:9100/ answered 200). Now --bind 127.0.0.1 keeps
# the port local; without it the default stays all interfaces (Docker, direct serving).
# Usage: bind_test.sh <lk-fcgi>   (needs curl; the host needs a non-loopback address)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7631
T=$(mktemp -d); trap '{ kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
pid=""
printf 'route("GET", "/", fn() => response::text("ok"))\n' > "$T/app.lk"
EXT=$(hostname -I 2>/dev/null | awk '{print $1}')
[ -n "$EXT" ] || { echo "FAIL: no non-loopback address on this host"; exit 1; }
fail=0
start() {   # start [env...] -- [args...]
    { kill $pid; wait $pid; } 2>/dev/null
    local envs=() args=()
    while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done; [ "${1:-}" = -- ] && shift
    ( cd "$T" && exec env "${envs[@]}" "$LKF" --mode http --port $P --workers 2 "$@" app.lk >"$T/log" 2>&1 ) & pid=$!
    for i in $(seq 1 40); do curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$P/" && return 0
        kill -0 $pid 2>/dev/null || return 1; sleep 0.2; done; return 1
}
reach() { curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://$1:$P/" || true; }
check() { if [ "$2" = "$3" ]; then echo "  OK   $1"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi; }

start X=1 -- || { echo "FAIL: did not start"; cat "$T/log"; exit 1; }
check "default: reachable on 127.0.0.1"           "$(reach 127.0.0.1)" 200
check "default: reachable on $EXT (unchanged)"     "$(reach "$EXT")" 200

start X=1 -- --bind 127.0.0.1 || { echo "FAIL: --bind 127.0.0.1 did not start"; cat "$T/log"; exit 1; }
check "--bind 127.0.0.1: reachable on 127.0.0.1"   "$(reach 127.0.0.1)" 200
check "--bind 127.0.0.1: not reachable on $EXT"    "$(reach "$EXT")" 000
grep -q "Listening on 127.0.0.1:$P" "$T/log" && echo "  OK   the log names the address" || { echo "  FAIL the log does not name 127.0.0.1:$P"; fail=1; }

start LOOK_BIND=127.0.0.1 -- || { echo "FAIL: LOOK_BIND did not start"; cat "$T/log"; exit 1; }
check "LOOK_BIND=127.0.0.1: not reachable on $EXT" "$(reach "$EXT")" 000

if start X=1 -- --bind localhost; then echo "  FAIL --bind localhost started (a name must be refused)"; fail=1
else grep -q "invalid --bind / LOOK_BIND address 'localhost'" "$T/log" && echo "  OK   --bind localhost: LOOK does not start and says why" \
     || { echo "  FAIL --bind localhost: no reason given"; tail -3 "$T/log"; fail=1; }; fi

[ $fail = 0 ] && echo "PASS: --mode http listens only where --bind says" || echo "FAIL: --bind"
exit $fail

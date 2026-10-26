#!/usr/bin/env bash
# The default session directory is one no other user can create first.
#
# 1.0.38 put sessions in ${TMPDIR:-/tmp}/look-sessions-<uid> and refused that directory when it
# belonged to someone else. Another user could still create it first in the shared /tmp: the
# site stayed up, but every request that used sessions failed (logins stopped working). Now the
# default is the first of: $STATE_DIRECTORY/sessions (systemd StateDirectory=),
# $XDG_RUNTIME_DIR/look-sessions, $HOME/.look/sessions (when $HOME belongs to this user), and
# only then /tmp/look-sessions-<uid>. LOOK_SESSION_DIR still wins.
# "Another user" is uid 12345 (needs root or passwordless sudo for chown).
# Usage: session_dir_default_test.sh <lk-fcgi>   (needs curl)
set -u
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7621
T=$(mktemp -d); trap '{ kill $pid; wait; } 2>/dev/null; ${SUDO} rm -rf "$T"' EXIT
pid=""
if [ "$(id -u)" = 0 ]; then SUDO=""; elif sudo -n true 2>/dev/null; then SUDO="sudo -n"; else
    echo "FAIL: needs root or passwordless sudo to create a directory owned by another user"; exit 1; fi
OTHER=12345; ME=$(id -u)
mkdir -p "$T/tmp" && chmod 1777 "$T/tmp"
cat > "$T/app.lk" <<'LK'
route("GET", "/login", function() { session::start(); session::set("user", "alice"); return response::text("ok") })
route("GET", "/who", function() { session::start(); return response::text("user=" . session::get("user")) })
LK
fail=0
# run <beklenen dizin> <açıklama> <ortam...>
run() {
    local want="$1" what="$2"; shift 2
    { kill $pid; wait $pid; } 2>/dev/null
    ( cd "$T" && exec env -u STATE_DIRECTORY -u XDG_RUNTIME_DIR -u LOOK_SESSION_DIR -u HOME \
        TMPDIR="$T/tmp" LOOK_SESSION_SECURE=0 "$@" "$LKF" --mode http --port $P --workers 2 app.lk >"$T/srv.log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P/health" >/dev/null && break; sleep 0.2; done
    rm -f "$T/cj"
    local code; code=$(curl -s -c "$T/cj" -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$P/login")
    local who; who=$(curl -s -b "$T/cj" --max-time 5 "http://127.0.0.1:$P/who")
    local sid; sid=$(awk '/LOOK_SESSION/{print $7}' "$T/cj")
    if [ "$code" = 200 ] && [ "$who" = "user=alice" ] && [ -f "$want/look_sess_$sid" ] && [ "$(stat -c %a "$want")" = 700 ]; then
        echo "  OK   $what → ${want#$T/}"
    else
        local got; got=$(find "$T" -name "look_sess_$sid" 2>/dev/null | head -1)
        echo "  FAIL $what: want ${want#$T/}, login HTTP $code, '$who', file at ${got#$T/}"; tail -2 "$T/srv.log"; fail=1
    fi
}
mkdir -m 0700 "$T/state";  run "$T/state/sessions"     "STATE_DIRECTORY (systemd)"      STATE_DIRECTORY="$T/state"
mkdir -m 0700 "$T/xdg";    run "$T/xdg/look-sessions"  "XDG_RUNTIME_DIR"                XDG_RUNTIME_DIR="$T/xdg"
mkdir -m 0755 "$T/home";   run "$T/home/.look/sessions" "HOME owned by this user"       HOME="$T/home"
mkdir -m 0755 "$T/rhome"; ${SUDO} chown $OTHER "$T/rhome"
                           run "$T/tmp/look-sessions-$ME" "HOME owned by another user → /tmp" HOME="$T/rhome"
# Saldırgan /tmp'deki dizini önceden yarattı: HOME varken girişler yine çalışmalı
${SUDO} rm -rf "$T/tmp/look-sessions-$ME"; mkdir -m 0777 "$T/tmp/look-sessions-$ME"; ${SUDO} chown $OTHER "$T/tmp/look-sessions-$ME"
mkdir -m 0755 "$T/home2";  run "$T/home2/.look/sessions" "logins work although another user created /tmp/look-sessions-<uid>" HOME="$T/home2"
[ $fail = 0 ] && echo "PASS: the default session directory is one no other user can create first" \
              || echo "FAIL: default session directory"
exit $fail

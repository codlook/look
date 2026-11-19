#!/usr/bin/env bash
# Session files: nobody else can read them, and a file LOOK did not write is never a session.
#
# Up to 1.0.37 the file driver wrote sessions to /tmp/look_sess_<id> with mode 0644. /tmp can be
# listed by every user: another user on the server could read the ids and contents and steal a
# session, and a file they wrote there themselves (look_sess_<id> with user=admin) was accepted
# as a session when its id came in the cookie. Now sessions live in a directory owned by the
# process's user with mode 0700 (here the last fallback, ${TMPDIR:-/tmp}/look-sessions-<uid> — HOME,
# XDG_RUNTIME_DIR and STATE_DIRECTORY are unset; or LOOK_SESSION_DIR), files
# are 0600 and never followed through a link, and a file that is not this user's 0600 file is
# ignored. "Another user" here is uid 12345 (needs root or passwordless sudo for chown).
# Usage: session_file_security_test.sh <lk-fcgi>   (needs curl)
set -u
# an app that uses cookies does not start without LOOK_SECRET (CSRF signature) — the test's own key.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
P=7601
T=$(mktemp -d); trap '{ kill $pid; wait; } 2>/dev/null; ${SUDO} rm -rf "$T"' EXIT
pid=""
if [ "$(id -u)" = 0 ]; then SUDO=""; elif sudo -n true 2>/dev/null; then SUDO="sudo -n"; else
    echo "FAIL: needs root or passwordless sudo to create files owned by another user"; exit 1; fi
OTHER=12345
SHARED="$T/shared"; mkdir -p "$SHARED"; chmod 1777 "$SHARED"     # like /tmp
cat > "$T/app.lk" <<'LK'
route("GET", "/who", function() { session::start(); return response::text("user=" . session::get("user")) })
route("GET", "/login", function() { session::start(); session::set("user", "alice"); return response::text("ok") })
LK
fail=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
start() {   # $@ = extra environment
    { kill $pid; wait $pid; } 2>/dev/null
    ( cd "$T" && exec env -u HOME -u XDG_RUNTIME_DIR -u STATE_DIRECTORY TMPDIR="$SHARED" TEMP="$SHARED" LOOK_SESSION_SECURE=0 "$@" \
        "$LKF" --mode http --port $P --workers 2 app.lk >"$T/srv.log" 2>&1 ) & pid=$!
    for i in $(seq 1 50); do curl -s --max-time 1 "http://127.0.0.1:$P/health" >/dev/null && return 0
        kill -0 $pid 2>/dev/null || return 1; sleep 0.2; done; return 1
}
who() { curl -s --max-time 5 -b "LOOK_SESSION=$1" "http://127.0.0.1:$P/who"; }
plant() {   # plant <file> — a forged session written by another user
    printf 'user=admin\n' > "$1"; chmod 0644 "$1"; ${SUDO} chown $OTHER:$OTHER "$1"
}
UIDME=$(id -u)
SID=ffffffffffffffffffffffffffffffff

# 1. A forged file left in the shared directory (the old place)
plant "$SHARED/look_sess_$SID"
start || { echo "FAIL: server did not start"; cat "$T/srv.log"; exit 1; }
r=$(who $SID); [ "$r" = "user=admin" ] && bad "a file another user put in the shared temp directory was accepted as a session ($r)" \
    || ok "a file another user put in the shared temp directory is not a session ($r)"

# 2. A real session: the file 0600, the directory 0700, nobody else can read it
curl -s -c "$T/cj" --max-time 5 "http://127.0.0.1:$P/login" >/dev/null
real=$(awk '/LOOK_SESSION/{print $7}' "$T/cj")
f=$(find "$SHARED" -name "look_sess_$real" 2>/dev/null | head -1)
if [ -z "$f" ]; then bad "the session file was not found under the temp directory"; else
    fm=$(stat -c %a "$f"); dm=$(stat -c %a "$(dirname "$f")"); du=$(stat -c %u "$(dirname "$f")")
    if [ "$fm" = 600 ] && [ "$dm" = 700 ] && [ "$du" = "$UIDME" ] && [ "$(dirname "$f")" != "$SHARED" ]; then
        ok "session file $fm in a private directory ($dm, owner uid $du)"
    else bad "session file mode $fm in $(dirname "$f") (mode $dm, owner uid $du) — others can read it"; fi
fi
SD="$SHARED/look-sessions-$UIDME"

# 3. A file belonging to someone else in our own directory → not a session
if [ -d "$SD" ]; then
    plant "$SD/look_sess_$SID"
    r=$(who $SID); [ "$r" = "user=admin" ] && bad "a file owned by another user in the session directory was accepted ($r)" \
        || ok "a file owned by another user in the session directory is ignored ($r)"
    ${SUDO} rm -f "$SD/look_sess_$SID"
    # 4. A symbolic link is not followed
    printf 'user=admin\n' > "$T/target"; chmod 600 "$T/target"; ln -s "$T/target" "$SD/look_sess_$SID"
    r=$(who $SID); [ "$r" = "user=admin" ] && bad "a symbolic link in the session directory was followed ($r)" \
        || ok "a symbolic link in the session directory is not followed ($r)"
    rm -f "$SD/look_sess_$SID"
else bad "no private session directory $SD"; fi

# 5. A default directory the attacker created beforehand → not used, the request gets an explicit error
{ kill $pid; wait $pid; } 2>/dev/null
rm -rf "$SD"; mkdir -p "$SD"; chmod 0777 "$SD"; plant "$SD/look_sess_$SID"; ${SUDO} chown $OTHER:$OTHER "$SD"
start || { bad "server did not start with a hijacked default directory (apps without sessions must still run)"; }
r=$(who $SID); code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -b "LOOK_SESSION=$SID" "http://127.0.0.1:$P/who")
if [ "$r" != "user=admin" ] && [ "$code" = 500 ] && grep -q "refusing to use it" "$T/srv.log"; then
    ok "a session directory owned by another user is refused (HTTP $code, reason logged)"
else bad "a session directory owned by another user: got '$r' (HTTP $code)"; fi
${SUDO} rm -rf "$SD"

# 6. LOOK_SESSION_DIR belongs to someone else or has loose permissions → LOOK does not start
mkdir -p "$T/other_dir" "$T/loose_dir"; ${SUDO} chown $OTHER "$T/other_dir"; chmod 0755 "$T/loose_dir"
for d in other_dir loose_dir; do
    if start LOOK_SESSION_DIR="$T/$d"; then bad "LOOK started with LOOK_SESSION_DIR=$d (owner/mode not checked)"
    else grep -q "could not start: session directory" "$T/srv.log" && ok "LOOK_SESSION_DIR=$d: LOOK does not start and says why" \
        || { bad "LOOK_SESSION_DIR=$d: did not start, but no reason"; tail -3 "$T/srv.log"; }; fi
done
mkdir -p "$T/good_dir"; chmod 0700 "$T/good_dir"
if start LOOK_SESSION_DIR="$T/good_dir"; then
    curl -s -c "$T/cj2" --max-time 5 "http://127.0.0.1:$P/login" >/dev/null
    n=$(find "$T/good_dir" -name 'look_sess_*' -perm 600 | wc -l)
    [ "$n" -ge 1 ] && ok "LOOK_SESSION_DIR is used (0600 files in it)" || bad "LOOK_SESSION_DIR is not used"
else bad "LOOK did not start with a good LOOK_SESSION_DIR"; tail -3 "$T/srv.log"; fi

[ $fail = 0 ] && echo "PASS: session files are private and a foreign file is never a session" \
              || echo "FAIL: session files"
exit $fail

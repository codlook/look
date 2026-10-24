#!/usr/bin/env bash
# The Plesk extension runs each domain's LOOK service as that domain's system user, never as root.
#
# enable.sh wrote a systemd unit without User=: every site added from the panel ran as root, so
# any bug in a site's code (or in LOOK) was a root bug, and one site could read every other
# site's files and passwords. It also ignored the script chosen in the panel, created the sample
# index.lk as root, and removing a domain left its unit file and its .service.d folder behind.
#
# This test runs the real enable.sh/disable.sh with stubbed systemctl/ss/lk-fcgi in a throwaway
# container (it creates users and writes under /etc, /opt and /var/www — never run it on a host).
# Usage (repository root):
#   docker run --rm -v "$PWD:/w" -w /w almalinux:8 bash cpp/tests/plesk_service_user_test.sh
set -u
if [ ! -f /.dockerenv ] && [ "${LOOK_PLESK_SANDBOX:-}" != 1 ]; then
    echo "refusing to run outside a container (set LOOK_PLESK_SANDBOX=1 to override)"; exit 2
fi
[ "$(id -u)" = 0 ] || { echo "must run as root inside the container"; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../platforms/plesk/htdocs/scripts"
fails=0
ok()   { echo "  OK   $1"; }
bad()  { echo "  FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# --- sandbox: plugin scripts, stub binaries, a Plesk-like webspace ---------------------------
PLUG=/tmp/plesk-mod/scripts; STUB=/tmp/plesk-stub; LOG=/tmp/plesk-systemctl.log
rm -rf /tmp/plesk-mod "$STUB" /var/www/vhosts /etc/systemd/system/look-*; : > "$LOG"
mkdir -p "$PLUG" "$STUB" /opt/look /etc/systemd/system
cp "$SRC"/*.sh "$PLUG/"; chmod 644 "$PLUG"/*.sh            # Plesk unpacks the zip without exec bits
cat > "$STUB/sudo" <<'EOF'
#!/bin/bash
case "$1" in /bin/systemctl) shift; echo "systemctl $*" >> /tmp/plesk-systemctl.log; exit 0 ;; esac
exec "$@"
EOF
printf '#!/bin/bash\necho "LISTEN 0 128 127.0.0.1:9150 0.0.0.0:*"\n' > "$STUB/ss"
chmod 755 "$STUB/sudo" "$STUB/ss"
# lk-fcgi stand-in: prints who runs it, its arguments and the variables the app would see
cat > /opt/look/lk-fcgi <<'EOF'
#!/bin/bash
[ "${1:-}" = --version ] && { echo "LOOK 1.0.99 (abcdef0, linux/amd64)"; exit 0; }
echo "USER=$(id -un)"; echo "ARGS=$*"; echo "DB_DSN=${DB_DSN-<unset>}"; echo "APP_NAME=${APP_NAME-<unset>}"
echo "LOOK_FILE_ROOT=${LOOK_FILE_ROOT-<unset>}"
EOF
chmod 755 /opt/look/lk-fcgi
export PATH="$STUB:$PATH"

getent group psacln >/dev/null || groupadd psacln
getent group psaserv >/dev/null || groupadd psaserv
id site.com_a1 >/dev/null 2>&1 || useradd -M -d /var/www/vhosts/site.com -s /sbin/nologin -g psacln -u 10051 site.com_a1
id other.com_b2 >/dev/null 2>&1 || useradd -M -d /var/www/vhosts/other.com -s /sbin/nologin -g psacln -u 10052 other.com_b2
DOC=/var/www/vhosts/site.com/app.site.com
mkdir -p "$DOC" /var/www/vhosts/other.com/httpdocs /var/www/vhosts/rootowned.com/httpdocs
chown site.com_a1:psaserv "$DOC"; chown other.com_b2:psaserv /var/www/vhosts/other.com/httpdocs
chmod 750 "$DOC" /var/www/vhosts/other.com/httpdocs

enable()  { bash "$PLUG/enable.sh" "$@" 2>&1; }
disable() { bash "$PLUG/disable.sh" "$@" 2>&1; }
UNIT=/etc/systemd/system/look-app-site-com.service
# Runs the unit's ExecStart the way systemd would: as User=, in WorkingDirectory=, with Environment=
run_unit() {
    local u wd cmd envs
    u="$(sed -n 's/^User=//p' "$UNIT")"; wd="$(sed -n 's/^WorkingDirectory=//p' "$UNIT")"
    cmd="$(sed -n 's/^ExecStart=//p' "$UNIT")"; envs="$(sed -n 's/^Environment=//p' "$UNIT" | tr '\n' ' ')"
    runuser -u "${u:-root}" -- env -i PATH=/usr/bin:/bin $envs bash -c "cd '$wd' && exec $cmd" 2>&1
}

echo "== the service runs as the domain's user =="
printf 'DB_DSN=mysql://app:p#ss@localhost/app\n# a comment\n\nAPP_NAME="My Site"\n' > "$DOC/.look.env"
chown site.com_a1:psacln "$DOC/.look.env"; chmod 600 "$DOC/.look.env"
out="$(enable app.site.com "$DOC/app.lk" 2 http 9150 "$DOC/.look.env")"
check "enable succeeds ($(echo "$out" | tail -1))" '[[ "$out" == *OK:look-app-site-com:9150* ]]'
check "unit has User=site.com_a1"  'grep -qx "User=site.com_a1" "$UNIT"'
check "unit has Group=psacln"      'grep -qx "Group=psacln" "$UNIT"'
check "unit has no User=root"      '! grep -q "^User=root" "$UNIT"'
check "the sample app is owned by the domain's user" '[ "$(stat -c %U "$DOC/app.lk" 2>/dev/null)" = site.com_a1 ]'
check "the sample app does not claim a version" '! grep -q "1\.0\.0" "$DOC/app.lk"'
run="$(run_unit)"
check "the process runs as site.com_a1"          '[[ "$run" == *USER=site.com_a1* ]]'
check "the chosen script is started"             '[[ "$run" == *"--script $DOC/app.lk"* ]]'
check "the env file is loaded (a # inside a value is kept)" '[[ "$run" == *"DB_DSN=mysql://app:p#ss@localhost/app"* ]]'
check "quotes around a value are removed"        '[[ "$run" == *"APP_NAME=My Site"* ]]'
check "LOOK_FILE_ROOT is the document root"      '[[ "$run" == *"LOOK_FILE_ROOT=$DOC"* ]]'

echo "== the env file is read with the domain user's rights =="
printf 'DB_DSN=mysql://other:secret@localhost/other\n' > /var/www/vhosts/other.com/httpdocs/.look.env
chown other.com_b2:psacln /var/www/vhosts/other.com/httpdocs/.look.env; chmod 600 /var/www/vhosts/other.com/httpdocs/.look.env
ln -sf /var/www/vhosts/other.com/httpdocs/.look.env "$DOC/.look.env"
run="$(run_unit)"
check "a symlink to another site's env file gives nothing" '[[ "$run" != *other:secret* ]]'
check "and the service does not start"                     '[[ "$run" != *USER=* ]]'
rm -f "$DOC/.look.env"

echo "== warning for an inline comment =="
printf 'APP_NAME=Shop   # shown in the title\n' > "$DOC/.look.env"; chown site.com_a1:psacln "$DOC/.look.env"
run="$(run_unit)"
check "a value with ' #' is reported" '[[ "$run" == *"inline comments are not supported"* ]]'

echo "== what enable.sh refuses =="
rm -f /etc/systemd/system/look-*.service
out="$(enable rootowned.com /var/www/vhosts/rootowned.com/httpdocs/index.lk 2 http 9150)"
check "a document root owned by root is refused" '[[ "$out" == *ERROR* ]] && [ ! -f /etc/systemd/system/look-rootowned-com.service ]'
out="$(enable app.site.com /var/www/vhosts/other.com/httpdocs/index.lk 2 http 9150)"
check "a script outside the domain's document root is refused" '[[ "$out" == *ERROR* ]] && [ ! -f "$UNIT" ]'
out="$(enable app.site.com "$DOC/app.lk" 2 http 9150 /var/www/vhosts/other.com/httpdocs/.look.env)"
check "an env file outside the domain's document root is refused" '[[ "$out" == *ERROR* ]] && [ ! -f "$UNIT" ]'
out="$(enable app.site.com "$DOC/app.lk" 2 http 9150 "$DOC/missing.env")"
check "a missing env file is refused with its name" '[[ "$out" == *ERROR*missing.env* ]] && [ ! -f "$UNIT" ]'
out="$(enable app.site.com "$DOC/app.lk" 2 http 80)"
check "a port below 1024 is refused" '[[ "$out" == *ERROR* ]] && [ ! -f "$UNIT" ]'
rm -f "$DOC/new.lk"; ln -s /etc/look-pwn.lk "$DOC/new.lk"
out="$(enable app.site.com "$DOC/new.lk" 2 http 9150)"
check "the sample is never written through a symlink as root" '[ ! -e /etc/look-pwn.lk ]'
rm -f "$DOC/new.lk"

echo "== stop keeps the unit, remove deletes it =="
enable app.site.com "$DOC/app.lk" 2 http 9150 >/dev/null
mkdir -p "$UNIT.d"; echo "[Service]" > "$UNIT.d/override.conf"
disable look-app-site-com app.site.com >/dev/null
check "stop leaves the unit file" '[ -f "$UNIT" ]'
: > "$LOG"
disable look-app-site-com app.site.com remove >/dev/null
check "remove deletes the unit file"        '[ ! -e "$UNIT" ]'
check "remove deletes the .service.d folder" '[ ! -e "$UNIT.d" ]'
check "remove reloads systemd"              'grep -q "daemon-reload" "$LOG"'
check "remove leaves the site's files"      '[ -f "$DOC/app.lk" ]'

echo
if [ "$fails" = 0 ]; then echo "PASS: LOOK services run as the domain's user"; exit 0; fi
echo "FAIL: $fails check(s)"; exit 1

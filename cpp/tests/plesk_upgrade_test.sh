#!/usr/bin/env bash
# Updating the Plesk extension updates LOOK and rewrites every domain's service.
#
# Plesk runs the extension's post-install hook on install AND on upgrade, but as psaadm, not
# root (measured on Plesk Obsidian: euid 983). The hook ran setup.sh, which could not write
# /opt/look, sudoers or the units, and its errors were hidden — so updating the extension left
# the old LOOK binary in place and every service written by 1.1.x kept running as root.
# This test plays the upgrade: services written by 1.1.0 (no User=), the old binary running,
# then the hook's root step. Run in a throwaway container (it creates users, writes /etc, /opt):
#   docker run --rm -v "$PWD:/w" -w /w almalinux:8 bash cpp/tests/plesk_upgrade_test.sh
set -u
if [ ! -f /.dockerenv ] && [ "${LOOK_PLESK_SANDBOX:-}" != 1 ]; then
    echo "refusing to run outside a container (set LOOK_PLESK_SANDBOX=1 to override)"; exit 2
fi
[ "$(id -u)" = 0 ] || { echo "must run as root inside the container"; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
PL="$HERE/../../platforms/plesk"
fails=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

MOD=/usr/local/psa/admin/htdocs/modules/look-lang
LOG=/tmp/plesk-systemctl.log
SYS=/etc/systemd/system

reset_sandbox() {
    pkill -x lk-fcgi 2>/dev/null; sleep 0.2
    rm -rf "$MOD" /opt/look /var/www/vhosts "$SYS"/look-* /etc/sudoers.d/look-lang; : > "$LOG"
    mkdir -p "$MOD/bin" /opt/look "$SYS" /etc/sudoers.d
    cp -r "$PL/htdocs/." "$MOD/"; chmod 644 "$MOD"/scripts/*.sh   # Plesk unpacks without exec bits
    # systemctl / ss / sudo stand-ins (systemd does not run in this container)
    printf '#!/bin/bash\necho "systemctl $*" >> %s\nexit 0\n' "$LOG" > /usr/bin/systemctl
    printf '#!/bin/bash\nexec "$@"\n' > /usr/local/bin/sudo
    printf '#!/bin/bash\necho "LISTEN 0 128 127.0.0.1:9150 0.0.0.0:*"\necho "LISTEN 0 128 127.0.0.1:9151 0.0.0.0:*"\n' > /usr/local/bin/ss
    chmod 755 /usr/bin/systemctl /usr/local/bin/sudo /usr/local/bin/ss
    # The new release's LOOK, bundled in the extension
    printf '#!/bin/bash\necho "LOOK 1.0.99 (abcdef0, linux/amd64)"\n' > "$MOD/bin/lk"
    cp "$MOD/bin/lk" "$MOD/bin/lk-fcgi"
}

old_unit() { # <domain> <docroot> <port> — what extension 1.1.0 wrote: no User=
    local svc="look-$(echo "$1" | tr . -)"
    cat > "$SYS/$svc.service" <<EOF
[Unit]
Description=LOOK FastCGI - $1
After=network.target mariadb.service mysqld.service

[Service]
Type=simple
WorkingDirectory=$2
ExecStart=/opt/look/lk-fcgi --mode http --port $3 --workers 2
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal
Environment=LOOK_FILE_ROOT=$2

[Install]
WantedBy=multi-user.target
EOF
}

# The hook's root step: 1.0.33+ sbin/look-setup with the registered domains (post-install.php
# passes them); before that the hook ran htdocs/scripts/setup.sh (here even as root, its best case).
hook() {
    if [ -f "$PL/sbin/look-setup" ]; then bash "$PL/sbin/look-setup" "$@" 2>&1
    else bash "$MOD/scripts/setup.sh" 2>&1; fi
}

getent group psacln >/dev/null || groupadd psacln
id site.com_a1 >/dev/null 2>&1 || useradd -M -s /sbin/nologin -g psacln -u 10051 site.com_a1

echo "== upgrade from 1.1.0 with services running =="
reset_sandbox
A=/var/www/vhosts/site.com/app.site.com; B=/var/www/vhosts/rootsite.com/httpdocs
mkdir -p "$A" "$B"; chown site.com_a1:psacln "$A"
printf 'route("GET", "/", fn() => "a")\n' > "$A/index.lk"; chown site.com_a1:psacln "$A/index.lk"
old_unit app.site.com "$A" 9150
old_unit rootsite.com "$B" 9151
old_unit orphan.com /var/www/vhosts/orphan.com/httpdocs 9152
# the old LOOK is running: a real executable (cp onto it fails with "Text file busy")
cp /bin/sleep /opt/look/lk-fcgi; cp /bin/sleep /opt/look/lk
(exec -a lk-fcgi /opt/look/lk-fcgi 300) & OLD=$!
sleep 0.3
out="$(hook app.site.com "$A/index.lk" 2 http 9150 "" rootsite.com "$B/index.lk" 2 http 9151 "")"; rc=$?
kill "$OLD" 2>/dev/null
echo "$out" | sed 's/^/       | /'
check "the running LOOK binary is replaced"            '/opt/look/lk-fcgi --version 2>/dev/null | grep -q "LOOK 1.0.99"'
check "the registered domain now runs as its user"     'grep -qx "User=site.com_a1" "$SYS/look-app-site-com.service"'
check "its chosen script is started"                   'grep -q -- "--script $A/index.lk" "$SYS/look-app-site-com.service"'
check "a domain that cannot leave root is stopped"     'grep -q "stop look-rootsite-com" "$LOG" && grep -q "disable look-rootsite-com" "$LOG"'
check "an unregistered root service is stopped"        'grep -q "stop look-orphan-com" "$LOG"'
check "both are named in the output"                   '[[ "$out" == *rootsite.com*STOPPED* ]] && [[ "$out" == *look-orphan-com*STOPPED* ]]'
check "the hook reports failure (exit $rc)"            '[ "$rc" != 0 ]'
check "sudoers is written, mode 440"                   '[ "$(stat -c %a /etc/sudoers.d/look-lang 2>/dev/null)" = 440 ] && grep -q "$MOD/scripts/enable.sh" /etc/sudoers.d/look-lang'

echo "== a clean upgrade succeeds =="
reset_sandbox
mkdir -p "$A"; chown site.com_a1:psacln "$A"
old_unit app.site.com "$A" 9150
out="$(hook app.site.com "$A/index.lk" 2 http 9150 "")"; rc=$?
check "exit 0 when every service could be rewritten" '[ "$rc" = 0 ]'
check "the service runs as the domain's user"       'grep -qx "User=site.com_a1" "$SYS/look-app-site-com.service"'

echo "== failures are not silent =="
reset_sandbox
rm -f "$MOD"/bin/*
out="$(hook)"; rc=$?
check "no LOOK binary at all: non-zero exit with a message" '[ "$rc" != 0 ] && [[ "$out" == *ERROR* ]]'

echo "== the hook runs the root step through Plesk's root channel =="
PI="$PL/plib/scripts/post-install.php"
check "post-install.php calls pm_ApiCli::callSbin('look-setup')" 'grep -q "callSbin('"'"'look-setup'"'"'" "$PI"'
check "post-install.php exits non-zero when setup fails"         'grep -q "exit(1)" "$PI"'
check "the package ships sbin/look-setup"                        'grep -q "sbin" "$PL/build.sh" && [ -f "$PL/sbin/look-setup" ]'

echo
if [ "$fails" = 0 ]; then echo "PASS: updating the extension updates LOOK and every service"; exit 0; fi
echo "FAIL: $fails check(s)"; exit 1

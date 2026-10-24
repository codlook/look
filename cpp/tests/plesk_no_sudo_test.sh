#!/usr/bin/env bash
# The Plesk extension gives the panel user no sudo rights; root work goes through one checked script.
#
# The extension wrote /etc/sudoers.d/look-lang so the panel (psaadm) could run systemctl and its
# scripts as root. In sudoers a '*' also matches spaces: "systemctl enable look-*" allowed
# "systemctl enable look-a /tmp/evil.service" and "systemctl stop look-a sshd" — any code that
# runs as psaadm (a panel or extension bug) could register its own unit as root or stop any
# service. Now the panel calls sbin/look-ctl through Plesk's root channel (pm_ApiCli::callSbin),
# which checks the action, the service name (^look-[a-z0-9-]+$) and the argument count, and the
# setup removes the sudoers file.
# Run in a throwaway container (it installs sudo, creates users, writes /etc):
#   docker run --rm -v "$PWD:/w" -w /w almalinux:8 bash cpp/tests/plesk_no_sudo_test.sh
set -u
if [ ! -f /.dockerenv ] && [ "${LOOK_PLESK_SANDBOX:-}" != 1 ]; then
    echo "refusing to run outside a container (set LOOK_PLESK_SANDBOX=1 to override)"; exit 2
fi
[ "$(id -u)" = 0 ] || { echo "must run as root inside the container"; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
PL="$HERE/../../platforms/plesk"
MOD=/usr/local/psa/admin/htdocs/modules/look-lang
CTL="$PL/sbin/look-ctl"
fails=0
ok()  { echo "  OK   $1"; }
bad() { echo "  FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

command -v sudo >/dev/null || dnf install -y -q sudo >/dev/null 2>&1
id psaadm >/dev/null 2>&1 || useradd -M -s /sbin/nologin psaadm
rm -rf "$MOD" /opt/look; mkdir -p "$MOD/bin" /opt/look /etc/sudoers.d
cp -r "$PL/htdocs/." "$MOD/"
printf '#!/bin/bash\necho "LOOK 1.0.99"\n' > "$MOD/bin/lk"; cp "$MOD/bin/lk" "$MOD/bin/lk-fcgi"
printf '#!/bin/bash\necho "systemctl $*" >> /tmp/systemctl.log\n[ "$1" = is-active ] && echo active\nexit 0\n' > /usr/bin/systemctl
chmod 755 /usr/bin/systemctl; : > /tmp/systemctl.log

echo "== after install or update, psaadm has no sudo rights =="
# the rule 1.0.33 wrote is on the server before the update
cat > /etc/sudoers.d/look-lang <<EOF
psaadm ALL=(root) NOPASSWD: /bin/bash $MOD/scripts/enable.sh *, /bin/systemctl enable look-*, /bin/systemctl stop look-*
EOF
chmod 440 /etc/sudoers.d/look-lang
bash "$PL/sbin/look-setup" > /tmp/setup.out 2>&1
allowed() { sudo -n -l -U psaadm "$@" > /dev/null 2>&1; }
check "psaadm cannot run: systemctl enable look-a /tmp/evil.service" '! allowed /bin/systemctl enable look-a /tmp/evil.service'
check "psaadm cannot run: systemctl stop look-a sshd"                 '! allowed /bin/systemctl stop look-a sshd'
check "psaadm cannot run the extension scripts with sudo"            '! allowed /bin/bash $MOD/scripts/enable.sh x'
check "the sudoers file is gone"                                      '[ ! -e /etc/sudoers.d/look-lang ]'

echo "== the panel and the scripts do not use sudo =="
check "index.php calls look-ctl through pm_ApiCli::callSbin" 'grep -q "callSbin('"'"'look-ctl'"'"'" "$PL/htdocs/index.php"'
check "index.php runs no sudo command"                        '! grep -vE "^[[:space:]]*(//|\*|#)" "$PL/htdocs/index.php" | grep -qE "sudo "'
check "the scripts run no sudo command"                       '! grep -qE "^[^#]*\bsudo " "$PL"/htdocs/scripts/*.sh'

echo "== look-ctl checks what it is asked =="
run_ctl() { [ -f "$CTL" ] && bash "$CTL" "$@" > /dev/null 2>&1; }
check "status look-a: accepted"                       'run_ctl status look-a'
check "start look-a: accepted"                        'run_ctl start look-a'
check "start look-a sshd: refused (one service only)" '! run_ctl start look-a sshd'
check "start sshd: refused (not a LOOK service)"      '! run_ctl start sshd'
check "start look-../x: refused"                      '! run_ctl start look-../x'
check "start LOOK-A: refused (lowercase only)"        '! run_ctl start LOOK-A'
check "logs look-a abc: refused (count is a number)"  '! run_ctl logs look-a abc'
check "disable look-a d.com wipe: refused"            '! run_ctl disable look-a d.com wipe'
check "an unknown action is refused"                  '! run_ctl reboot'
check "nothing but look-a reached systemctl"          '! grep -vE " look-a$" /tmp/systemctl.log | grep -q .'

echo
if [ "$fails" = 0 ]; then echo "PASS: the panel user has no sudo rights; root work goes through look-ctl"; exit 0; fi
echo "FAIL: $fails check(s)"; exit 1

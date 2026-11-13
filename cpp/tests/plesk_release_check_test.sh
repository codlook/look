#!/usr/bin/env bash
# The Plesk extension moves a site to a new LOOK release only after `lk --check` of the new release
# passes on the site's code. A site whose code does not pass keeps running the release it ran
# before, and the panel says why — with the lines to fix, and that the site receives no fixes
# (security fixes included) until they are fixed. Nobody picks a version: every site that passes
# is on the newest release. A release no service uses any more is removed.
# Run in a throwaway container (it creates users, writes /etc, /opt):
#   docker run --rm -v "$PWD:/w" -w /w almalinux:8 bash cpp/tests/plesk_release_check_test.sh
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
SYS=/etc/systemd/system
ST=/opt/look/conf/status

# A stand-in LOOK of a given release. --check fails on a line containing the release's breaking
# marker (BREAKS-<version>) with LOOK's own "CHECK <line> <col> <message>" output, and records
# who ran it — the check must run as the site's user.
fake_look() { # <dir> <version> <stamp>
    mkdir -p "$1"
    cat > "$1/lk" <<EOF
#!/bin/bash
if [ "\${1:-}" = --version ]; then echo "LOOK $2 ($3, linux/amd64)"; exit 0; fi
if [ "\${1:-}" = --check ]; then
    echo "\$(id -un)" >> /tmp/check-users.log
    n=\$(grep -n "BREAKS-$2" "\$2" | head -1 | cut -d: -f1)
    if [ -n "\$n" ]; then echo "CHECK \$n 3 [compile] line \$n: this code does not load on LOOK $2"; exit 1; fi
    echo OK; exit 0
fi
exit 0
EOF
    chmod 755 "$1/lk"; cp "$1/lk" "$1/lk-fcgi"; cp "$1/lk" "$1/lk-cgi"
}

reset_sandbox() {
    rm -rf "$MOD" /opt/look /var/www/vhosts "$SYS"/look-* /tmp/check-users.log
    mkdir -p "$MOD" /opt/look "$SYS"
    cp -r "$PL/htdocs/." "$MOD/"; chmod 644 "$MOD"/scripts/*.sh
    printf '#!/bin/bash\nexit 0\n' > /usr/bin/systemctl
    printf '#!/bin/bash\necho "LISTEN 0 128 127.0.0.1:9150 0.0.0.0:*"\n' > /usr/local/bin/ss
    chmod 755 /usr/bin/systemctl /usr/local/bin/ss
    : > /tmp/check-users.log; chmod 666 /tmp/check-users.log
}
legacy_unit() { # <domain> <port> — a service written before releases existed: runs /opt/look/lk-fcgi
    local svc="look-$(echo "$1" | tr . -)"
    printf '[Unit]
Description=LOOK FastCGI - %s

[Service]
User=%s
ExecStart=/opt/look/lk-fcgi --mode http --bind 127.0.0.1 --port %s --workers 2 --script /var/www/vhosts/%s/httpdocs/index.lk
'         "$1" "$(stat -c %U /var/www/vhosts/$1/httpdocs)" "$2" "$1" > "$SYS/$svc.service"
}
site() { # <domain> <user> <uid> <code> — a site under /var/www/vhosts owned by its user
    getent group psacln >/dev/null || groupadd psacln
    id "$2" >/dev/null 2>&1 || useradd -M -d "/var/www/vhosts/$1" -s /sbin/nologin -g psacln -u "$3" "$2"
    mkdir -p "/var/www/vhosts/$1/httpdocs"; chown -R "$2":psacln "/var/www/vhosts/$1"
    printf '%s\n' "$4" > "/var/www/vhosts/$1/httpdocs/index.lk"; chown "$2":psacln "/var/www/vhosts/$1/httpdocs/index.lk"
}
unit_bin() { sed -n 's#^ExecStart=\(.* \)\{0,1\}\(/opt/look/[^ ]*lk-fcgi\) .*#\2#p' "$SYS/look-$(echo "$1" | tr . -).service"; }
# Plesk unpacks the extension without exec bits — the bundled binaries are not executable
setup() { chmod 644 "$MOD"/bin/* 2>/dev/null; bash "$PL/sbin/look-setup" "$@" 2>&1; }
args_for() { echo "$1 /var/www/vhosts/$1/httpdocs/index.lk 2 http $2"; }

echo "== upgrade from an extension without releases: one site passes, one does not =="
reset_sandbox
site good.test good_u 10061 'route("GET", "/", fn() => "ok")'
site old.test  old_u  10062 $'route("GET", "/", fn() => "ok")\n$x = 1  # BREAKS-1.0.99'
fake_look /tmp/rel-old 1.0.47 1b846a3; cp /tmp/rel-old/lk* /opt/look/       # what the sites run today
legacy_unit good.test 9150; legacy_unit old.test 9151
fake_look "$MOD/bin" 1.0.99 abcdef0                                          # the extension's new LOOK
out="$(setup $(args_for good.test 9150) "" $(args_for old.test 9151) "")"; rc=$?
echo "$out" | sed 's/^/       | /'
check "the hook succeeds (a held site is not a failure)"      '[ "$rc" = 0 ]'
check "both releases are kept side by side"                   '[ -x /opt/look/releases/1.0.47-1b846a3/lk-fcgi ] && [ -x /opt/look/releases/1.0.99-abcdef0/lk-fcgi ]'
check "/opt/look/lk is the newest release"                    '/opt/look/lk --version | grep -q "1.0.99 (abcdef0"'
check "the site that passes runs the new release"             '[ "$(unit_bin good.test)" = /opt/look/releases/1.0.99-abcdef0/lk-fcgi ]'
check "the site that does not pass keeps the release it ran"  '[ "$(unit_bin old.test)" = /opt/look/releases/1.0.47-1b846a3/lk-fcgi ]'
check "the check ran as the user of each site"               'grep -qx good_u /tmp/check-users.log && grep -qx old_u /tmp/check-users.log && ! grep -qx root /tmp/check-users.log'
check "the held site is named in the output, with its line"   '[[ "$out" == *old.test*1.0.47*1.0.99*"line 2"* ]]'
check "status: held, on 1.0.47, newest 1.0.99, with the check output" \
      'grep -q "\"held\": *true" $ST/look-old-test.json && grep -q "1.0.47-1b846a3" $ST/look-old-test.json && grep -q "1.0.99-abcdef0" $ST/look-old-test.json && grep -q "CHECK 2 3" $ST/look-old-test.json'
check "status: the passing site is not held"                  'grep -q "\"held\": *false" $ST/look-good-test.json'
check "the status files are readable by the panel"            '[ "$(stat -c %a $ST/look-old-test.json)" = 644 ]'

echo "== the site fixes its code and is checked again =="
printf '%s\n' 'route("GET", "/", fn() => "fixed")' > /var/www/vhosts/old.test/httpdocs/index.lk
out="$(bash "$MOD/scripts/enable.sh" $(args_for old.test 9151) 2>&1)"; rc=$?
check "enable (the restart in the panel) succeeds"                 '[ "$rc" = 0 ]'
check "the site moves to the new release"                     '[ "$(unit_bin old.test)" = /opt/look/releases/1.0.99-abcdef0/lk-fcgi ]'
check "status: no longer held"                                'grep -q "\"held\": *false" $ST/look-old-test.json'
out="$(setup $(args_for good.test 9150) "" $(args_for old.test 9151) "")"
check "the next setup removes the release no service uses"    '[ ! -e /opt/look/releases/1.0.47-1b846a3 ] && [ -x /opt/look/releases/1.0.99-abcdef0/lk-fcgi ]'

echo "== a new site that does not pass is not installed =="
site new.test new_u 10063 $'$y = 2  # BREAKS-1.0.99'
out="$(bash "$MOD/scripts/enable.sh" $(args_for new.test 9152) 2>&1)"; rc=$?
check "enable fails with the check output"                    '[ "$rc" != 0 ] && [[ "$out" == *ERROR*"line 1"* ]]'
check "no service is written for it"                          '[ ! -e "$SYS/look-new-test.service" ]'

echo "== a held site stays held across two more releases until it passes =="
reset_sandbox
site old.test old_u 10062 $'$x = 1  # BREAKS-1.0.99 BREAKS-1.1.0'
fake_look /tmp/rel-old 1.0.47 1b846a3; cp /tmp/rel-old/lk* /opt/look/; legacy_unit old.test 9151
fake_look "$MOD/bin" 1.0.99 abcdef0; setup $(args_for old.test 9151) "" > /dev/null
fake_look "$MOD/bin" 1.1.0 1234567; out="$(setup $(args_for old.test 9151) "")"
check "still on the release it ran, not the one in between"   '[ "$(unit_bin old.test)" = /opt/look/releases/1.0.47-1b846a3/lk-fcgi ]'
check "the unused in-between release is removed"              '[ ! -e /opt/look/releases/1.0.99-abcdef0 ] && [ -x /opt/look/releases/1.1.0-1234567/lk-fcgi ]'
check "status names the newest release"                       'grep -q "1.1.0-1234567" $ST/look-old-test.json'

echo "== a new release without lk cannot check a site: the site stays where it is =="
reset_sandbox
site good.test good_u 10061 'route("GET", "/", fn() => "ok")'
fake_look /tmp/rel-old 1.0.47 1b846a3; cp /tmp/rel-old/lk* /opt/look/; legacy_unit good.test 9150
fake_look "$MOD/bin" 1.0.99 abcdef0; rm -f "$MOD/bin/lk"                       # a broken package
out="$(setup $(args_for good.test 9150) "")"; rc=$?
check "the site keeps the release it ran (not moved unchecked)" '[ "$(unit_bin good.test)" = /opt/look/releases/1.0.47-1b846a3/lk-fcgi ]'
check "status: held, saying it could not be checked"           'grep -q "\"held\": *true" $ST/look-good-test.json && grep -q "could not be checked" $ST/look-good-test.json'
check "the output names it"                                    '[[ "$out" == *good.test*"could not be checked"* ]]'
site new.test new_u 10063 'route("GET", "/", fn() => "ok")'
out="$(bash "$MOD/scripts/enable.sh" $(args_for new.test 9152) 2>&1)"; rc=$?
check "a new site is not installed unchecked"                  '[ "$rc" != 0 ] && [ ! -e "$SYS/look-new-test.service" ]'

echo "== the panel shows it =="
P="$PL/htdocs/phtml/index.phtml"
check "the panel reads the release status"                    'grep -q "release_status" "$PL/htdocs/index.php"'
check "a held site is told it gets no security fixes"         'grep -q "receives no fixes, security fixes included" "$P"'
check "the panel shows the lines to fix"                      'grep -q "check_lines" "$P"'
check "the panel offers to check again"                       'grep -q "Check again" "$P"'
if command -v php >/dev/null; then
    check "index.php parses" 'php -l "$PL/htdocs/index.php" >/dev/null'
    check "index.phtml parses" 'php -l "$P" >/dev/null'
fi

[ "$fails" = 0 ] && echo "PASS: a site moves to a new release only when its code passes the check" || { echo "FAIL: $fails"; exit 1; }

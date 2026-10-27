#!/usr/bin/env bash
# The Linux installer stops on an option it does not know, and says which one.
#
# install.sh took no options but ignored any it was given. The Ubuntu guide told users to run it
# with --with-service --with-nginx --with-cli: the script went ahead without a word, the user
# believed nginx had been set up in front of the app, and the app's port stayed open on every
# interface. Now an unknown option stops it before anything is installed.
#
# Runs the real install.sh with stub binaries in a throwaway container (it writes /usr/local/bin,
# /etc/look, /var/www and creates a user — never run it on a host). Usage (repository root):
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:24.04 bash cpp/tests/linux_install_test.sh
set -u
if [ ! -f /.dockerenv ] && [ "${LOOK_INSTALL_SANDBOX:-}" != 1 ]; then
    echo "refusing to run outside a container (set LOOK_INSTALL_SANDBOX=1 to override)"; exit 2
fi
[ "$(id -u)" = 0 ] || { echo "must run as root inside the container"; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
INST="$HERE/../../platforms/linux/ubuntu/install.sh"
fails=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }

STUB=$(mktemp -d)
for b in lk lk-fcgi lk-cgi; do printf '#!/bin/sh\necho "LOOK 1.0.99 (stub)"\n' > "$STUB/$b"; chmod 755 "$STUB/$b"; done
command -v curl >/dev/null 2>&1 || { printf '#!/bin/sh\nexit 7\n' > /usr/local/bin/curl; chmod 755 /usr/local/bin/curl; }
clean() { rm -rf /usr/local/bin/lk /usr/local/bin/lk-fcgi /usr/local/bin/lk-cgi /etc/look /var/www/look /etc/systemd/system/look.service; }
installed() { [ -e /usr/local/bin/lk-fcgi ] || [ -e /etc/look ] || [ -e /etc/systemd/system/look.service ]; }

echo "== an option install.sh does not have"
for opt in --with-nginx --with-service --port; do
    clean
    out="$(LOOK_BIN_SRC="$STUB" bash "$INST" $opt 2>&1)"; rc=$?
    check "$opt: stops with an error (exit $rc)"        '[ "$rc" != 0 ]'
    check "$opt: names the option"                     '[[ "$out" == *"unknown option: $opt"* ]]'
    check "$opt: installs nothing"                     '! installed'
done

echo "== --help"
clean
out="$(bash "$INST" --help 2>&1)"; rc=$?
check "--help exits 0"                                 '[ "$rc" = 0 ]'
check "--help names the settings (LOOK_PORT, LOOK_BIND)" '[[ "$out" == *LOOK_PORT* && "$out" == *LOOK_BIND* ]]'
check "--help installs nothing"                        '! installed'

echo "== no options: installs as before"
clean
out="$(LOOK_BIN_SRC="$STUB" bash "$INST" 2>&1)"; rc=$?
check "exit 0 ($(echo "$out" | tail -1 | cut -c1-60))" '[ "$rc" = 0 ]'
check "lk-fcgi installed"                              '[ -x /usr/local/bin/lk-fcgi ]'
check "the unit runs as user look"                     'grep -qx "User=look" /etc/systemd/system/look.service'
# look.env DB_DSN parolası taşır: yalnız root okur (eskiden 0644, her kullanıcı okurdu).
check "look.env is mode 0600, owned by root"           '[ "$(stat -c %a:%U /etc/look/look.env)" = 600:root ]'

clean; rm -rf "$STUB"
[ $fails = 0 ] && echo "PASS: install.sh refuses options it does not have" || { echo "FAIL: $fails check(s)"; exit 1; }

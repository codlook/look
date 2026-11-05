#!/usr/bin/env bash
# The Plesk extension keeps a site's secrets out of its web root and gives LOOK 2 its LOOK_SECRET.
#
# 1. The env file had to be inside the document root. A disabled site's proxy rule is removed and
#    Apache then serves the document root as plain files — /.look.env with the database password
#    could be downloaded. Now the env file may live anywhere in the domain user's own home (outside
#    the web root), and a disabled site keeps a rule that refuses /.look* files.
# 2. LOOK 2 does not start an application that uses cookies without LOOK_SECRET. enable.sh creates
#    one when the site has none: as the domain's user (root never writes into the user's home —
#    a symlink there could otherwise point root at any file), from /dev/urandom, 0600, outside the
#    web root, and never printed. An existing secret is kept; one set in the site's own env file wins.
# The privilege model stays: no sudo, the service never runs as root.
# Usage (repository root):
#   docker run --rm -v "$PWD:/w" -w /w almalinux:8 bash cpp/tests/plesk_env_secret_test.sh
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

PLUG=/tmp/plesk-mod/scripts; STUB=/tmp/plesk-stub
rm -rf /tmp/plesk-mod "$STUB" /var/www/vhosts /etc/systemd/system/look-*
mkdir -p "$PLUG" "$STUB" /opt/look /etc/systemd/system
cp "$SRC"/*.sh "$PLUG/"; chmod 644 "$PLUG"/*.sh
printf '#!/bin/bash\nexit 0\n' > /usr/bin/systemctl
printf '#!/bin/bash\necho "LISTEN 0 128 127.0.0.1:9160 0.0.0.0:*"\n' > "$STUB/ss"
chmod 755 /usr/bin/systemctl "$STUB/ss"
# lk-fcgi stand-in: who runs it and what it sees (the secret only as its length and a hash prefix)
cat > /opt/look/lk-fcgi <<'EOF'
#!/bin/bash
echo "USER=$(id -un)"; echo "DB_DSN=${DB_DSN-<unset>}"
if [ -n "${LOOK_SECRET:-}" ]; then echo "SECRET_LEN=${#LOOK_SECRET}"; echo "SECRET_HASH=$(printf %s "$LOOK_SECRET" | sha256sum | cut -c1-12)"; else echo "SECRET_LEN=0"; fi
EOF
chmod 755 /opt/look/lk-fcgi
export PATH="$STUB:$PATH"

getent group psacln >/dev/null || groupadd psacln
getent group psaserv >/dev/null || groupadd psaserv
id site.com_a1 >/dev/null 2>&1 || useradd -M -d /var/www/vhosts/site.com -s /sbin/nologin -g psacln -u 10061 site.com_a1
id other.com_b2 >/dev/null 2>&1 || useradd -M -d /var/www/vhosts/other.com -s /sbin/nologin -g psacln -u 10062 other.com_b2
HOME_A=/var/www/vhosts/site.com; DOC=$HOME_A/httpdocs
mkdir -p "$DOC" /var/www/vhosts/other.com/httpdocs
chown site.com_a1:psaserv "$HOME_A" "$DOC"; chown other.com_b2:psaserv /var/www/vhosts/other.com /var/www/vhosts/other.com/httpdocs
chmod 750 "$HOME_A" "$DOC" /var/www/vhosts/other.com /var/www/vhosts/other.com/httpdocs

enable()  { bash "$PLUG/enable.sh" "$@" 2>&1; }
disable() { bash "$PLUG/disable.sh" "$@" 2>&1; }
UNIT=/etc/systemd/system/look-site-com.service
run_unit() {
    local u wd cmd envs
    u="$(sed -n 's/^User=//p' "$UNIT")"; wd="$(sed -n 's/^WorkingDirectory=//p' "$UNIT")"
    cmd="$(sed -n 's/^ExecStart=//p' "$UNIT")"; envs="$(sed -n 's/^Environment=//p' "$UNIT" | tr '\n' ' ')"
    runuser -u "${u:-root}" -- env -i PATH=/usr/bin:/bin $envs bash -c "cd '$wd' && exec $cmd" 2>&1
}
SECRET=$HOME_A/.look/look-site-com.secret

echo "== an env file outside the web root =="
mkdir -p "$HOME_A/private"; printf 'DB_DSN=mysql://app:pw@localhost/app\n' > "$HOME_A/private/site.env"
chown -R site.com_a1:psacln "$HOME_A/private"; chmod 700 "$HOME_A/private"; chmod 600 "$HOME_A/private/site.env"
out="$(enable site.com "$DOC/index.lk" 2 http 9160 "$HOME_A/private/site.env")"
check "enable accepts an env file in the user's home, outside the web root ($(echo "$out" | tail -1))" '[[ "$out" == *OK:look-site-com:9160* ]]'
run="$(run_unit)"
check "the service reads it (DB_DSN set)" '[[ "$run" == *DB_DSN=mysql://app:pw@localhost/app* ]]'
check "the service still runs as the domain user" '[[ "$run" == *USER=site.com_a1* ]] && grep -qx "User=site.com_a1" "$UNIT"'
printf 'DB_DSN=x\n' > /var/www/vhosts/other.com/o.env; chown other.com_b2:psacln /var/www/vhosts/other.com/o.env; chmod 600 /var/www/vhosts/other.com/o.env
out="$(enable site.com "$DOC/index.lk" 2 http 9160 /var/www/vhosts/other.com/o.env)"
check "another subscription's env file is refused" '[[ "$out" != *OK:* ]]'
out="$(enable site.com "$DOC/index.lk" 2 http 9160 /etc/passwd)"
check "a file outside /var/www/vhosts is refused" '[[ "$out" != *OK:* ]]'

echo "== LOOK_SECRET =="
rm -f "$SECRET"
out="$(enable site.com "$DOC/index.lk" 2 http 9160 "$HOME_A/private/site.env")"
check "a secret file is created" '[ -f "$SECRET" ]'
check "… owned by the domain user, mode 600" '[ "$(stat -c %U:%a "$SECRET" 2>/dev/null)" = site.com_a1:600 ]'
check "… in a 700 folder outside the web root" '[ "$(stat -c %a "$HOME_A/.look" 2>/dev/null)" = 700 ] && [[ "$SECRET" != "$DOC"/* ]]'
key="$(sed -n 's/^LOOK_SECRET=//p' "$SECRET" 2>/dev/null)"
check "… 64 hex characters (32 random bytes)" '[[ "$key" =~ ^[0-9a-f]{64}$ ]]'
check "the secret is not printed by enable" '[ -n "$key" ] && [[ "$out" != *"$key"* ]]'
check "the secret is not in the unit file (world-readable)" '[ -n "$key" ] && ! grep -q "$key" "$UNIT"'
run="$(run_unit)"
h="$(printf %s "$key" | sha256sum | cut -c1-12)"
check "the service gets it (LOOK_SECRET of 64 characters)" '[[ "$run" == *SECRET_LEN=64* ]] && [[ "$run" == *SECRET_HASH=$h* ]]'
enable site.com "$DOC/index.lk" 2 http 9160 "$HOME_A/private/site.env" >/dev/null
check "a second enable keeps the same secret" '[ "$(sed -n "s/^LOOK_SECRET=//p" "$SECRET")" = "$key" ]'
out="$(enable site.com "$DOC/index.lk" 2 http 9160)"
run="$(run_unit)"
check "without an env file the service still gets the secret" '[[ "$run" == *SECRET_HASH=$h* ]]'
printf 'LOOK_SECRET=%s\n' "own-secret-0123456789abcdef0123456789abcdef" >> "$HOME_A/private/site.env"
enable site.com "$DOC/index.lk" 2 http 9160 "$HOME_A/private/site.env" >/dev/null
run="$(run_unit)"
h2="$(printf %s own-secret-0123456789abcdef0123456789abcdef | sha256sum | cut -c1-12)"
check "a LOOK_SECRET in the site's own env file wins" '[[ "$run" == *SECRET_HASH=$h2* ]]'

echo "== root never writes into the user's home =="
rm -rf "$HOME_A/.look"; ln -s /etc "$HOME_A/.look"; chown -h site.com_a1:psacln "$HOME_A/.look"
enable site.com "$DOC/index.lk" 2 http 9160 >/dev/null
check ".look pointing at /etc: nothing is written in /etc" '[ ! -e /etc/look-site-com.secret ]'
rm -f "$HOME_A/.look"

echo "== a disabled site does not serve its .look files =="
enable site.com "$DOC/index.lk" 2 http 9160 >/dev/null
disable look-site-com site.com >/dev/null
CONF=/var/www/vhosts/system/site.com/conf/vhost.conf
check "after disable the site's Apache config refuses /.look* files" 'grep -q "FilesMatch" "$CONF" 2>/dev/null && grep -q "Require all denied" "$CONF"'
check "… and no longer proxies to the stopped service" '! grep -q "ProxyPass /" "$CONF" 2>/dev/null'
check "… the same for HTTPS" 'grep -q "Require all denied" /var/www/vhosts/system/site.com/conf/vhost_ssl.conf 2>/dev/null'
check "no sudo in any command of the scripts (comments aside)" '! grep -nE "^[^#]*\bsudo " "$PLUG"/*.sh'

echo
[ $fails = 0 ] && echo "PASS: secrets stay out of the web root; LOOK 2 gets its LOOK_SECRET" || echo "FAIL: $fails"
exit $fails

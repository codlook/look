#!/bin/bash
# LOOK Language — disable a domain
# Usage: disable.sh <svc_name> [<domain>] [remove]
# 'remove' (removing a domain in the panel): also deletes the unit file and the <svc>.service.d folder;
# otherwise (Stop) the unit stays and can be started again with Start.

set -e

SVC_NAME="$1"
DOMAIN="$2"
REMOVE="$3"

if [ -z "$SVC_NAME" ]; then
    echo "Usage: $0 <svc_name> [<domain>] [remove]" >&2
    exit 1
fi

# SECURITY: SVC_NAME goes to sudo systemctl stop/disable. Without the look-* restriction
# the panel user could stop an ARBITRARY service (sshd, mariadb) as root (multi-tenant
# DoS). Require look-* + narrow to valid unit characters. DOMAIN also goes into a file path/rm.
SVC_NAME=$(printf '%s' "$SVC_NAME" | tr -cd 'A-Za-z0-9.-')
DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
case "$SVC_NAME" in
    look-*) ;;
    *) echo "ERROR: only look-* services allowed" >&2; exit 1 ;;
esac

/bin/systemctl stop "$SVC_NAME" 2>/dev/null || true
/bin/systemctl disable "$SVC_NAME" 2>/dev/null || true

if [ "$REMOVE" = "remove" ]; then
    rm -f "/etc/systemd/system/${SVC_NAME}.service"
    rm -rf "/etc/systemd/system/${SVC_NAME}.service.d"
    /bin/systemctl daemon-reload
fi

# vhost config: the Apache proxy rule is removed and a rule that denies /.look* files stays in its place.
# Files used to be deleted: Apache reverted to serving the web root as plain files and the /.look.env
# in the web root (database password) could be downloaded. The rule stays as long as the site is stopped; when the site is
# enabled again, enable.sh writes the files with the proxy rule (including the same deny rule).
if [ -n "$DOMAIN" ]; then
    CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
    mkdir -p "$CONF_DIR"
    DENY_CONF='<FilesMatch "^\.look">
    Require all denied
</FilesMatch>'
    for f in vhost.conf vhost_ssl.conf; do
        printf '%s\n' "$DENY_CONF" > "$CONF_DIR/$f"
    done

    # websrvmng does not exist in Plesk Obsidian; without httpdmng Apache keeps the old proxy even if vhost.conf is deleted
    if [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "$DOMAIN" >/dev/null 2>&1 || true
    elif [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="$DOMAIN" 2>/dev/null || true
    fi
fi

echo "OK:stopped:${SVC_NAME}"

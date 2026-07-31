#!/bin/bash
# LOOK Language — Domain devre disi birak
# Kullanim: disable.sh <svc_name> [<domain>]

set -e

SVC_NAME="$1"
DOMAIN="$2"

if [ -z "$SVC_NAME" ]; then
    echo "Kullanim: $0 <svc_name> [domain]" >&2
    exit 1
fi

sudo /bin/systemctl stop "$SVC_NAME" 2>/dev/null || true
sudo /bin/systemctl disable "$SVC_NAME" 2>/dev/null || true

# vhost config temizle (Apache proxy'yi kaldir)
if [ -n "$DOMAIN" ]; then
    CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
    for f in vhost.conf vhost_ssl.conf; do
        [ -f "$CONF_DIR/$f" ] && rm -f "$CONF_DIR/$f"
    done

    if [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        sudo /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="$DOMAIN" 2>/dev/null || true
    fi
fi

echo "OK:stopped:${SVC_NAME}"

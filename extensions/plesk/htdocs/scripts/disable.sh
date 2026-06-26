#!/bin/bash
# Kullanim: disable.sh <domain> [svc_name]
set -euo pipefail
DOMAIN="$1"
SVC_NAME="${2:-look-$(echo "$DOMAIN" | tr "[:upper:]" "[:lower:]" | sed "s/[^a-z0-9-]/-/g")}"
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"
NGINX_CONF="/var/www/vhosts/${DOMAIN}/conf/vhost_nginx.conf"
echo "[disable] Domain: $DOMAIN | Servis: $SVC_NAME"
systemctl stop    "$SVC_NAME" 2>/dev/null || true
systemctl disable "$SVC_NAME" 2>/dev/null || true
if [ -f "$SVC_FILE" ]; then rm -f "$SVC_FILE"; systemctl daemon-reload; fi
if [ -f "$NGINX_CONF" ] && grep -q "# LOOK" "$NGINX_CONF" 2>/dev/null; then
    rm -f "$NGINX_CONF"
    plesk sbin websrvmng --reconfigure-vhost --vhost-name="$DOMAIN" 2>/dev/null || true
fi
echo "[disable] OK"

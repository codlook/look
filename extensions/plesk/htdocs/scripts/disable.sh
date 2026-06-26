#!/bin/bash
# LOOK Language — Domain LOOK yapilandirmasini kaldir
# Kullanim: disable.sh <domain>
set -euo pipefail

DOMAIN="$1"
SVC_NAME="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9-]/-/g')"
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"
NGINX_CONF="/var/www/vhosts/${DOMAIN}/conf/vhost_nginx.conf"

echo "[disable] Domain: $DOMAIN | Servis: $SVC_NAME"

# --- Servisi durdur ve devre disi birak ---
if systemctl is-active "$SVC_NAME" &>/dev/null; then
    systemctl stop "$SVC_NAME"
    echo "[disable] Servis durduruldu: $SVC_NAME"
fi

if systemctl is-enabled "$SVC_NAME" &>/dev/null; then
    systemctl disable "$SVC_NAME"
    echo "[disable] Servis devre disi: $SVC_NAME"
fi

# --- Service dosyasini sil ---
if [ -f "$SVC_FILE" ]; then
    rm -f "$SVC_FILE"
    echo "[disable] Service dosyasi silindi: $SVC_FILE"
fi

systemctl daemon-reload

# --- nginx conf temizle ---
if [ -f "$NGINX_CONF" ]; then
    # LOOK bloklarini sil, diger icerik varsa koru
    if grep -q "# LOOK" "$NGINX_CONF"; then
        rm -f "$NGINX_CONF"
        echo "[disable] nginx conf silindi: $NGINX_CONF"
    fi
fi

# --- nginx reload ---
if command -v plesk &>/dev/null; then
    plesk sbin websrvmng --reconfigure-vhost --vhost-name="$DOMAIN" 2>/dev/null || \
    plesk sbin websrvmng -u 2>/dev/null || true
else
    nginx -s reload 2>/dev/null || true
fi

echo "[disable] OK: $DOMAIN LOOK yapilandirmasi kaldirildi."

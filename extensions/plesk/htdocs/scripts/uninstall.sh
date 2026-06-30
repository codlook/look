#!/bin/bash
# LOOK Language — Plesk extension kaldirma scripti
# Tum aktif domainleri durdurur, binary ve config dosyalarini siler
set -euo pipefail

CONF_DIR="/opt/look/conf"
BINARY_DIR="/opt/look"
LOG_DIR="/var/log/look"
DOMAINS_FILE="${CONF_DIR}/domains.json"

echo "[uninstall] LOOK extension kaldiriliyor..."

# Tum domainleri durdur
if [ -f "$DOMAINS_FILE" ] && command -v python3 &>/dev/null; then
    DOMAINS=$(python3 -c "
import json, sys
data = json.load(open('$DOMAINS_FILE'))
for d in data:
    print(d.get('domain',''))
" 2>/dev/null || true)
    for DOMAIN in $DOMAINS; do
        [ -z "$DOMAIN" ] && continue
        SVC="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9-]/-/g')"
        echo "[uninstall] Durduruluyor: $SVC"
        systemctl stop    "$SVC" 2>/dev/null || true
        systemctl disable "$SVC" 2>/dev/null || true
        rm -f "/etc/systemd/system/${SVC}.service"
    done
fi

systemctl daemon-reload

# Binary ve config sil
rm -rf "$BINARY_DIR"
echo "[uninstall] Binary silindi: $BINARY_DIR"

# Log dosyalarini koru (sadece logrotate config sil)
rm -f /etc/logrotate.d/look
rm -f /etc/profile.d/look.sh

echo "[uninstall] LOOK extension kaldirildi."

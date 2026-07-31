#!/bin/bash
# LOOK Language — Domain etkinlestirme
# Kullanim: enable.sh <domain> <script> <workers> <mode> <port>
# Ornek:    enable.sh look.codlook.com /var/www/.../index.lk 4 fcgi 9100

set -e

DOMAIN="$1"
SCRIPT="$2"
WORKERS="${3:-4}"
MODE="${4:-fcgi}"
PORT="${5:-9100}"

if [ -z "$DOMAIN" ] || [ -z "$SCRIPT" ] || [ -z "$PORT" ]; then
    echo "Usage: $0 <domain> <script> <workers> <mode> <port>" >&2
    exit 1
fi

LK_FCGI="/opt/look/lk-fcgi"
LK_BIN="/opt/look/lk"

# lk-fcgi binary kontrol
if [ ! -x "$LK_FCGI" ]; then
    echo "ERROR: $LK_FCGI not found or not executable" >&2
    exit 1
fi

# Servis adi: look.codlook.com -> look-look-codlook-com
SVC_NAME="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]/-/g')"

# Domain document root'unu bul — once hizli filesystem yolu. Standart Plesk yollari
# genelde tutar; tuttugunda 'plesk bin site --info' (~0.5sn) hic cagrilmaz.
# Ornek: look.codlook.com -> /var/www/vhosts/codlook.com/look.codlook.com/
DOC_ROOT=""
PARENT=$(echo "$DOMAIN" | sed 's/^[^.]*\.//')
if [ -d "/var/www/vhosts/$PARENT/$DOMAIN" ]; then
    DOC_ROOT="/var/www/vhosts/$PARENT/$DOMAIN"
elif [ -d "/var/www/vhosts/$DOMAIN/httpdocs" ]; then
    DOC_ROOT="/var/www/vhosts/$DOMAIN/httpdocs"
fi
# Fallback: Plesk API (yalnizca fs yolu tutmazsa — nadir)
if [ -z "$DOC_ROOT" ] && command -v plesk &>/dev/null; then
    DOC_ROOT=$(plesk bin site --info "$DOMAIN" 2>/dev/null | grep -i 'document root\|www_root\|httpdocs' | head -1 | awk '{print $NF}')
fi
if [ -z "$DOC_ROOT" ] || [ ! -d "$DOC_ROOT" ]; then
    DOC_ROOT=$(dirname "$SCRIPT")
fi

# Script dosyasi yoksa index.lk olustur
if [ ! -f "$SCRIPT" ]; then
    mkdir -p "$(dirname "$SCRIPT")"
    cat > "$SCRIPT" << 'INDEXLK'
# LOOK v1 — Plesk domain sample app (edit this file)
route("GET", "/", fn() => response::json(["ok" => true, "app" => "LOOK", "version" => "1.0.0"]))

route("GET", "/ping", fn() => response::json(["status" => "ok"]))

route("404", fn() => response::error(404, "Not found"))
INDEXLK
    echo "index.lk created: $SCRIPT"
fi

# ─── systemd servis dosyasi ────────────────────────────────────────────────
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"

if [ "$MODE" = "http" ]; then
    EXEC_CMD="$LK_FCGI --mode http --port $PORT --workers $WORKERS"
else
    EXEC_CMD="$LK_FCGI --port $PORT --workers $WORKERS"
fi

cat > "$SVC_FILE" << SYSTEMD
[Unit]
Description=LOOK FastCGI - ${DOMAIN}
After=network.target mariadb.service mysqld.service

[Service]
Type=simple
WorkingDirectory=${DOC_ROOT}
ExecStart=${EXEC_CMD}
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal
Environment=LOOK_FILE_ROOT=${DOC_ROOT}

[Install]
WantedBy=multi-user.target
SYSTEMD

sudo /bin/systemctl daemon-reload
sudo /bin/systemctl enable "$SVC_NAME" 2>/dev/null || true
sudo /bin/systemctl restart "$SVC_NAME"

# Port acilana kadar bekle (max 5s)
for i in $(seq 1 10); do
    if ss -tlnp | grep -q ":$PORT "; then
        break
    fi
    sleep 0.5
done

# ─── Apache vhost config ───────────────────────────────────────────────────
# Plesk: /var/www/vhosts/system/<domain>/conf/vhost.conf + vhost_ssl.conf
CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
mkdir -p "$CONF_DIR"

if [ "$MODE" = "http" ]; then
    # HTTP Proxy modu
    PROXY_CONF="ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / http://127.0.0.1:${PORT}/
ProxyPassReverse / http://127.0.0.1:${PORT}/"
else
    # FastCGI modu (ProxyPass ile — mod_proxy_fcgi)
    PROXY_CONF="ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / fcgi://127.0.0.1:${PORT}/
ProxyPassReverse / fcgi://127.0.0.1:${PORT}/"
fi

echo "$PROXY_CONF" > "$CONF_DIR/vhost.conf"
echo "$PROXY_CONF" > "$CONF_DIR/vhost_ssl.conf"

# Apache reconfigure (Plesk yolu) — ARKA PLANDA çalışır. Plesk reconfigure 10-30sn
# sürer; senkron çalışırsa panel AJAX'ı kilitler/timeout olur (domain görünmez).
# systemd servisi + vhost config zaten hazır → reconfigure birkaç saniye içinde
# uygular. Hemen OK dönüyoruz, UI anında domain'i listeye ekler.
nohup bash -c '
    if [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        sudo /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="'"$DOMAIN"'"
    elif [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        sudo /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "'"$DOMAIN"'"
    fi
' >/dev/null 2>&1 &

echo "OK:${SVC_NAME}:${PORT}"

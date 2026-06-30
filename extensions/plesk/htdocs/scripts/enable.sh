#!/bin/bash
# LOOK Language â€” Domain icin FastCGI servisi ve nginx config olustur
# Kullanim: enable.sh <domain> <script> <workers> <mode> <port>
set -euo pipefail

DOMAIN="$1"
SCRIPT="$2"
WORKERS="${3:-8}"
MODE="${4:-fcgi}"
PORT="${5:-9100}"

BINARY="/opt/look/lk-fcgi"
LOG_DIR="/var/log/look"
SVC_NAME="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9-]/-/g')"
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"
# Plesk: nginx config ve Apache custom config yerleri
# Standalone domain: /var/www/vhosts/${DOMAIN}/conf/
# Subdomain altinda: /var/www/vhosts/system/${DOMAIN}/conf/ (Apache custom config)
VHOST_DIR="/var/www/vhosts/${DOMAIN}/conf"
NGINX_CONF="${VHOST_DIR}/vhost_nginx.conf"
# Apache custom config (Plesk subdomain icin asil yol)
APACHE_CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
APACHE_VHOST_CONF="${APACHE_CONF_DIR}/vhost.conf"
APACHE_VHOST_SSL_CONF="${APACHE_CONF_DIR}/vhost_ssl.conf"
LOG_FILE="${LOG_DIR}/${DOMAIN}.log"

echo "[enable] Domain: $DOMAIN | Script: $SCRIPT | Workers: $WORKERS | Mode: $MODE | Port: $PORT"

# --- Script dosyasi yoksa otomatik olustur ---
if [ ! -f "$SCRIPT" ]; then
    SCRIPT_DIR_TMP=$(dirname "$SCRIPT")
    mkdir -p "$SCRIPT_DIR_TMP"
    cat > "$SCRIPT" << 'LOOKEOF'
route("GET", "/", function() {
    response::header("Content-Type", "application/json")
    print(json::encode(["ok" => true, "mesaj" => "LOOK calisiyor! index.lk dosyasini duzenle."]))
})

route("404", function() {
    response::status(404)
    response::header("Content-Type", "application/json")
    print(json::encode(["ok" => false, "hata" => "Sayfa bulunamadi"]))
})
LOOKEOF
    echo "[enable] Baslangic index.lk olusturuldu: $SCRIPT"
fi

# --- Binary kontrol ---
if [ ! -x "$BINARY" ]; then
    echo "[HATA] lk-fcgi bulunamadi: $BINARY" >&2
    exit 1
fi

# --- Log dizini ---
mkdir -p "$LOG_DIR"
touch "$LOG_FILE"
chown -R psaadm:psaadm "$LOG_DIR" 2>/dev/null || true

# --- systemd service dosyasi ---
SCRIPT_DIR=$(dirname "$SCRIPT")

cat > "$SVC_FILE" << EOF
[Unit]
Description=LOOK FastCGI â€” ${DOMAIN}
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${SCRIPT_DIR}
EOF

if [ "$MODE" = "http" ]; then
cat >> "$SVC_FILE" << EOF
ExecStart=${BINARY} --mode http --port ${PORT} --workers ${WORKERS} ${SCRIPT}
EOF
else
cat >> "$SVC_FILE" << EOF
ExecStart=${BINARY} --port ${PORT} --workers ${WORKERS} ${SCRIPT}
EOF
fi

cat >> "$SVC_FILE" << EOF
Restart=always
RestartSec=3
StandardOutput=append:${LOG_FILE}
StandardError=append:${LOG_FILE}
Environment=LOOK_ENV=production

[Install]
WantedBy=multi-user.target
EOF

echo "[enable] systemd service yazildi: $SVC_FILE"

# --- nginx vhost config (nginx dogrudan LOOK'a proxy oldugunda) ---
mkdir -p "$VHOST_DIR"

if [ "$MODE" = "fcgi" ]; then
cat > "$NGINX_CONF" << EOF
# LOOK FastCGI â€” ${DOMAIN}
# Otomatik olusturuldu: $(date)

location ~ \.lk$ {
    deny all;
}

location / {
    try_files \$uri \$uri/ @look;
}

location @look {
    include fastcgi_params;
    fastcgi_pass  127.0.0.1:${PORT};
    fastcgi_param SCRIPT_FILENAME ${SCRIPT};
    fastcgi_param REQUEST_URI     \$request_uri;
    fastcgi_param QUERY_STRING    \$query_string;
    fastcgi_param REQUEST_METHOD  \$request_method;
    fastcgi_param CONTENT_TYPE    \$content_type;
    fastcgi_param CONTENT_LENGTH  \$content_length;
    fastcgi_read_timeout 60;
}
EOF
else
cat > "$NGINX_CONF" << EOF
# LOOK HTTP â€” ${DOMAIN}
# Otomatik olusturuldu: $(date)

location ~ \.lk$ {
    deny all;
}

location / {
    proxy_pass         http://127.0.0.1:${PORT};
    proxy_http_version 1.1;
    proxy_set_header   Upgrade \$http_upgrade;
    proxy_set_header   Connection "upgrade";
    proxy_set_header   Host \$host;
    proxy_set_header   X-Real-IP \$remote_addr;
    proxy_set_header   X-Forwarded-For \$proxy_add_x_forwarded_for;
    proxy_read_timeout 3600;
    proxy_send_timeout 3600;
}
EOF
fi

echo "[enable] nginx conf yazildi: $NGINX_CONF"

# --- Apache custom config (Plesk: nginx â†’ Apache â†’ LOOK) ---
# Plesk'te nginx, istekleri Apache'ye (7081) proxy'lar.
# Apache custom config (vhost.conf + vhost_ssl.conf) bu akisi LOOK'a yonlendirir.
if [ -d "$APACHE_CONF_DIR" ]; then
    if [ "$MODE" = "fcgi" ]; then
        APACHE_PROXY="ProxyRequests Off\nProxyPreserveHost On\nProxyPass /.well-known !\nProxyPass / fcgi://127.0.0.1:${PORT}/\nProxyPassReverse / fcgi://127.0.0.1:${PORT}/"
    else
        APACHE_PROXY="ProxyRequests Off\nProxyPreserveHost On\nProxyPass /.well-known !\nProxyPass / http://127.0.0.1:${PORT}/\nProxyPassReverse / http://127.0.0.1:${PORT}/"
    fi
    printf "%b\n" "$APACHE_PROXY" > "$APACHE_VHOST_CONF"
    printf "%b\n" "$APACHE_PROXY" > "$APACHE_VHOST_SSL_CONF"
    echo "[enable] Apache vhost conf yazildi: $APACHE_CONF_DIR"
fi


# --- systemd reload ve baslat ---
if pidof systemd > /dev/null 2>&1 || [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    systemctl enable  "$SVC_NAME" 2>/dev/null || true
    systemctl restart "$SVC_NAME"

    # nginx + Apache reload — ARKA PLANDA (PHP timeout engeli)
    (
        sleep 1
        if command -v plesk &>/dev/null; then
            plesk sbin httpdmng --reconfigure-domain "$DOMAIN" 2>/dev/null || \
            plesk sbin websrvmng -u 2>/dev/null || true
        else
            nginx -s reload 2>/dev/null || true
        fi
    ) &

    sleep 1
    STATE=$(systemctl is-active "$SVC_NAME" 2>/dev/null || echo "unknown")
    if [ "$STATE" = "active" ]; then
        echo "[enable] OK: $SVC_NAME aktif (port $PORT)"
    else
        echo "[HATA] $SVC_NAME baslatılamadı (state: $STATE)" >&2
        systemctl status "$SVC_NAME" --no-pager -l 2>&1 | head -20 >&2
        exit 1
    fi
else
    # systemd yok (Docker/test) — look-fcgi dogrudan baslat
    echo "[enable] systemd yok — lk-fcgi dogrudan baslatiliyor (port $PORT)"
    pkill -f "lk-fcgi.*$PORT" 2>/dev/null || true
    sleep 0.5
    if [ "$MODE" = "http" ]; then
        nohup "$BINARY" --mode http --port "$PORT" --workers "$WORKERS" "$SCRIPT" >> "$LOG_FILE" 2>&1 &
    else
        nohup "$BINARY" --port "$PORT" --workers "$WORKERS" "$SCRIPT" >> "$LOG_FILE" 2>&1 &
    fi
    sleep 1
    if pgrep -f "lk-fcgi.*$PORT" > /dev/null 2>&1; then
        echo "[enable] OK: lk-fcgi port $PORT calisiyor"
    else
        echo "[HATA] lk-fcgi baslatılamadı" >&2
        exit 1
    fi
fi
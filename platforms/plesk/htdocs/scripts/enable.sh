#!/bin/bash
# LOOK Language — Domain etkinlestirme
# Kullanim: enable.sh <domain> <script> <workers> <mode> <port> [<env-file>]
# Ornek:    enable.sh look.codlook.com /var/www/.../index.lk 4 fcgi 9100 /var/www/.../.look.env
#
# Servis ASLA root calismaz: unit'e User= olarak domain'in sistem kullanicisi (document
# root'un sahibi) yazilir. Ortam dosyasini systemd (root) degil, o kullanici okur
# (look-env.sh) — EnvironmentFile= root ile okudugundan, site sahibi baska sitenin
# .look.env'ine symlink verip onun sifrelerini kendi sureci icine alabilirdi.

set -e

DOMAIN="$1"
SCRIPT="$2"
WORKERS="${3:-4}"
MODE="${4:-fcgi}"
PORT="${5:-9100}"
ENV_FILE="${6:-}"

if [ -z "$DOMAIN" ] || [ -z "$SCRIPT" ] || [ -z "$PORT" ]; then
    echo "Usage: $0 <domain> <script> <workers> <mode> <port> [<env-file>]" >&2
    exit 1
fi

# GUVENLIK (derinlemesine savunma): cagiranin (PHP) dogruladigina GUVENME. Bu script
# sudoers'ta '*' joker'iyle cagrilabilir ve icerigi asagida TIRNAKSIZ heredoc ile systemd
# unit'ine yaziliyor; sanitizasyon olmadan gomulu satir-sonu iceren bir arg, unit'e kendi
# direktifini enjekte edip 'sudo systemctl restart' ile root komut kosturur. Kaynagi ne
# olursa olsun arglari daralt:
DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
SCRIPT=$(printf '%s' "$SCRIPT" | tr -cd 'A-Za-z0-9._/-')
WORKERS=$(printf '%s' "$WORKERS" | tr -cd '0-9'); WORKERS="${WORKERS:-4}"
PORT=$(printf '%s' "$PORT" | tr -cd '0-9')
case "$MODE" in http|fcgi) ;; *) MODE="fcgi" ;; esac
[ -n "$DOMAIN" ] || { echo "ERROR: invalid domain" >&2; exit 1; }
[ -n "$PORT" ]   || { echo "ERROR: invalid port"   >&2; exit 1; }
case "$SCRIPT" in
    /var/www/vhosts/*) ;;
    *) echo "ERROR: script must be under /var/www/vhosts" >&2; exit 1 ;;
esac
case "$SCRIPT" in *..*) echo "ERROR: path traversal" >&2; exit 1 ;; esac
ENV_FILE=$(printf '%s' "$ENV_FILE" | tr -cd 'A-Za-z0-9._/-')
case "$ENV_FILE" in *..*) echo "ERROR: path traversal" >&2; exit 1 ;; esac
# Servis root olmadigindan 1024 alti portu acamaz — systemd'nin anlasilmaz hatasi yerine burada soyle
if [ "$PORT" -lt 1024 ] || [ "$PORT" -gt 65535 ]; then
    echo "ERROR: port must be between 1024 and 65535 (the service does not run as root)" >&2; exit 1
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
# DOC_ROOT de heredoc'a (WorkingDirectory/LOOK_FILE_ROOT) giriyor — Plesk API ciktisi veya
# dirname olsa bile son bir kez daralt (satir-sonu enjeksiyonuna karsi):
DOC_ROOT=$(printf '%s' "$DOC_ROOT" | tr -cd 'A-Za-z0-9._/-')
DOC_ROOT="${DOC_ROOT%/}"
case "$DOC_ROOT" in
    /var/www/vhosts/?*) ;;
    *) echo "ERROR: document root of $DOMAIN not found under /var/www/vhosts" >&2; exit 1 ;;
esac

# Servisin kullanicisi = document root'un sahibi (Plesk: domain'in sistem kullanicisi).
# Sahip root ya da bir sistem hesabi ise (uid < 1000) servisi KURMA — root'a dusmek yok.
RUN_UID=$(stat -c %u "$DOC_ROOT")
RUN_USER=$(stat -c %U "$DOC_ROOT")
if [ "$RUN_UID" -lt 1000 ] || [ "$(printf '%s' "$RUN_USER" | tr -cd 'A-Za-z0-9._-')" != "$RUN_USER" ]; then
    echo "ERROR: $DOC_ROOT is owned by '$RUN_USER'; a LOOK service runs as the domain's system user, never as root or a system account" >&2
    exit 1
fi
RUN_GROUP=$(id -gn "$RUN_USER")
RUN_GROUP=$(printf '%s' "$RUN_GROUP" | tr -cd 'A-Za-z0-9._-')

# Betik ve ortam dosyasi domain'in document root'u icinde olmali (baska sitenin dosyasi degil)
case "$SCRIPT" in
    "$DOC_ROOT"/*) ;;
    *) echo "ERROR: the script must be inside the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
esac
if [ -n "$ENV_FILE" ]; then
    case "$ENV_FILE" in
        "$DOC_ROOT"/*) ;;
        *) echo "ERROR: the env file must be inside the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
    esac
    # Okunabilirligi servisin kullanicisi olarak sina (root her dosyayi okur)
    if ! runuser -u "$RUN_USER" -- test -f "$ENV_FILE" -a -r "$ENV_FILE"; then
        echo "ERROR: env file not found or not readable by $RUN_USER: $ENV_FILE" >&2; exit 1
    fi
fi

# Script dosyasi yoksa ornek uygulama olustur — domain kullanicisi olarak (root bir
# symlink'i izleyip sistem dosyasi yazmasin; dosya da kullanicinin olsun ki duzenleyebilsin)
if [ ! -e "$SCRIPT" ] && [ ! -L "$SCRIPT" ]; then
    runuser -u "$RUN_USER" -- mkdir -p "$(dirname "$SCRIPT")"
    runuser -u "$RUN_USER" -- tee "$SCRIPT" > /dev/null << 'INDEXLK'
# LOOK — Plesk domain sample app (edit this file)
route("GET", "/", fn() => response::json(["ok" => true, "app" => "LOOK"]))

route("GET", "/ping", fn() => response::json(["status" => "ok"]))

route("404", fn() => response::error(404, "Not found"))
INDEXLK
    echo "sample app created: $SCRIPT"
fi

# ─── systemd servis dosyasi ────────────────────────────────────────────────
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"

if [ "$MODE" = "http" ]; then
    EXEC_CMD="$LK_FCGI --mode http --port $PORT --workers $WORKERS --script $SCRIPT"
else
    EXEC_CMD="$LK_FCGI --port $PORT --workers $WORKERS --script $SCRIPT"
fi
if [ -n "$ENV_FILE" ]; then
    # Ortam dosyasini servisin kullanicisi okur: look-env.sh root'a ait /opt/look'a kopyalanir
    install -m 755 "$(dirname "$0")/look-env.sh" /opt/look/look-env
    EXEC_CMD="/bin/bash /opt/look/look-env $ENV_FILE $EXEC_CMD"
fi

cat > "$SVC_FILE" << SYSTEMD
[Unit]
Description=LOOK FastCGI - ${DOMAIN}
After=network.target mariadb.service mysqld.service

[Service]
Type=simple
User=${RUN_USER}
Group=${RUN_GROUP}
NoNewPrivileges=true
PrivateTmp=true
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
    if [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        sudo /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "'"$DOMAIN"'"
    elif [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        sudo /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="'"$DOMAIN"'"
    fi
' >/dev/null 2>&1 &

# Eski surum servisi root ile calistiriyordu: onun yazdigi dosyalar (SQLite, yuklemeler) root'a
# ait kalmis olabilir ve servis artik onlara yazamaz. Otomatik chown yok (root olarak site
# agacinda gezinmek riskli) — yalniz soyle.
ROOT_FILES=$(find "$DOC_ROOT" -xdev -user root 2>/dev/null | head -1000 | wc -l)
if [ "$ROOT_FILES" -gt 0 ]; then
    echo "WARNING: $ROOT_FILES file(s) in $DOC_ROOT are owned by root; the service runs as $RUN_USER and cannot write them (find $DOC_ROOT -user root)"
fi

echo "OK:${SVC_NAME}:${PORT}"

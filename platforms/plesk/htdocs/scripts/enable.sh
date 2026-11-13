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

# LOOK sürümleri yan yana (sbin/look-setup): /opt/look/releases/<sürüm>-<damga>/, /opt/look/lk* en yeniyi
# gösterir. Site, kodu en yeni sürümün `lk --check`ini geçince ona bağlanır; geçemezse çalıştığı sürümde
# kalır (aşağıda). Eski düzende (releases yok) /opt/look/lk-fcgi düz dosyadır — o da çalışır.
OPT=/opt/look
REL="$OPT/releases"
STATUS_DIR="$OPT/conf/status"
LATEST_FCGI="$(readlink -f "$OPT/lk-fcgi" 2>/dev/null || true)"
if [ -z "$LATEST_FCGI" ] || [ ! -x "$LATEST_FCGI" ]; then
    echo "ERROR: $OPT/lk-fcgi not found or not executable" >&2
    exit 1
fi
LATEST_DIR="$(dirname "$LATEST_FCGI")"
# Sürüm adı: releases altındaki dizinin adı, değilse ikilinin --version'ından "<sürüm>-<damga>"
rel_name() {
    case "$1" in
        "$REL"/?*) basename "$1" ;;
        *) "$1/lk-fcgi" --version 2>/dev/null | head -1 | sed -n 's/^LOOK \([0-9][0-9A-Za-z.+-]*\) (\([0-9a-f]\{7,\}\)[,)].*/\1-\2/p' ;;
    esac
}
# JSON metni (durum dosyası): ters bölü, tırnak, satır sonu ve denetim karakterleri kaçışlı
json_str() { printf '%s' "$1" | tr -d '\r' | tr '\000-\010\013-\037' ' ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' | awk 'BEGIN{ORS=""} NR>1{print "\\n"} {print}'; }

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

# Oturum dosyalari: site kullanicisinin ev dizininde (web kokunun disinda), site basina, kalici.
# Servis PrivateTmp ile calisir; eskiden oturumlar ozel /tmp'de kaliyor, her yeniden baslatmada
# (eklenti guncellemesi dahil) dusuyordu. Dizini LOOK kendisi, site kullanicisi olarak 0700 ile
# yaratir — bu betik (root) kullanicinin ev dizininde hicbir sey yaratmaz/degistirmez.
# Ev dizini vhost altinda degilse (alisilmadik kurulum) satir yazilmaz: LOOK kendi varsayilan
# sirasini kullanir ($HOME bu kullaniciya aitse orada, degilse servisin ozel /tmp'si).
RUN_HOME=$(getent passwd "$RUN_USER" | cut -d: -f6 | tr -cd 'A-Za-z0-9._/-')
SESSION_ENV=""
case "$RUN_HOME" in
    /var/www/vhosts/?*) SESSION_ENV="Environment=LOOK_SESSION_DIR=${RUN_HOME%/}/.look/sessions-${SVC_NAME}" ;;
esac

# Betik ve ortam dosyasi domain'in document root'u icinde olmali (baska sitenin dosyasi degil)
case "$SCRIPT" in
    "$DOC_ROOT"/*) ;;
    *) echo "ERROR: the script must be inside the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
esac
if [ -n "$ENV_FILE" ]; then
    # Ortam dosyası domain kullanıcısının KENDİ alanında olmalı: ev dizini (web kökünün dışı —
    # önerilen) ya da document root. Eskiden yalnız document root kabul ediliyordu: site
    # durdurulunca Apache vekil kuralı kalkar ve web kökü düz dosya olarak sunulur — /.look.env
    # (veritabanı parolası) indirilebilirdi.
    case "$ENV_FILE" in
        "$DOC_ROOT"/*) ;;
        "${RUN_HOME%/}"/*) case "$RUN_HOME" in /var/www/vhosts/?*) ;; *) echo "ERROR: the env file must be inside the home of $RUN_USER or the document root of $DOMAIN" >&2; exit 1 ;; esac ;;
        *) echo "ERROR: the env file must be inside the home of $RUN_USER ($RUN_HOME) or the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
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

install -m 755 "$(dirname "$0")/look-env.sh" /opt/look/look-env
ENV_WRAP=""
if [ -n "$ENV_FILE" ]; then
    # Ortam dosyasini servisin kullanicisi okur: look-env.sh root'a ait /opt/look'a kopyalanir
    ENV_WRAP="/bin/bash /opt/look/look-env $ENV_FILE"
fi

# ─── LOOK_SECRET ───────────────────────────────────────────────────────────
# LOOK çerez kullanan uygulamayı LOOK_SECRET olmadan başlatmaz (CSRF imzası). Sitenin yoksa
# burada üretilir — domain kullanıcısı OLARAK (runuser): root kullanıcının ev dizinine hiçbir şey
# yazmaz (oradaki bir symlink root'u herhangi bir dosyaya yönlendirebilirdi). /dev/urandom'dan
# 32 bayt, 0700 klasörde 0600 dosya, web kökünün dışında; hiçbir çıktıya yazılmaz. Var olan
# korunur. Dıştaki look-env önce bunu, içteki sitenin kendi dosyasını yükler: sitenin kendi
# LOOK_SECRET'ı kazanır.
case "$RUN_HOME" in
    /var/www/vhosts/?*)
        SECRET_FILE="${RUN_HOME%/}/.look/${SVC_NAME}.secret"
        if runuser -u "$RUN_USER" -- /bin/bash -c '
                umask 077
                mkdir -p "$(dirname "$1")" 2>/dev/null || exit 1
                if [ ! -s "$1" ]; then
                    k=$(od -An -tx1 -N32 /dev/urandom | tr -d " \n")
                    [ ${#k} -eq 64 ] || exit 1
                    printf "LOOK_SECRET=%s\n" "$k" > "$1" || exit 1
                fi
                chmod 600 "$1"' _ "$SECRET_FILE" 2>/dev/null; then
            ENV_WRAP="/bin/bash /opt/look/look-env $SECRET_FILE${ENV_WRAP:+ $ENV_WRAP}"
        else
            echo "WARNING: could not create $SECRET_FILE as $RUN_USER; set LOOK_SECRET in the site's env file (LOOK needs it for applications that use cookies)"
        fi
        ;;
esac

# ─── Hangi LOOK sürümü? ────────────────────────────────────────────────────
# En yeni sürümün `lk --check`i sitenin koduyla — sitenin kullanıcısı olarak, kendi dizininde ve kendi
# ortamıyla (servisle aynı). --check yalnız yükler ve derler, programı ÇALIŞTIRMAZ (kurulum kodu,
# veritabanı geçişleri çalışmaz; tests/lk_check_no_side_effects_test.sh). Geçerse en yeni sürüm; geçemezse
# site çalıştığı sürümde kalır ve durum dosyası nedenini satır satır tutar — panel bunu "bu site güvenlik
# düzeltmeleri dahil hiçbir düzeltme almıyor" diye gösterir. Daha önce hiç çalışmamış (yeni) bir site
# geçemezse kurulmaz.
LATEST_REL="$(rel_name "$LATEST_DIR")"
CUR_FCGI=""
if [ -f "$SVC_FILE" ]; then
    CUR_FCGI="$(sed -n 's#^ExecStart=\(.* \)\{0,1\}\(/opt/look/[^ ]*lk-fcgi\) .*#\2#p' "$SVC_FILE" | head -1)"
    # Eklentinin sürümsüz düzeninde yazılmış servis /opt/look/lk-fcgi'yi çalıştırıyordu: o ikili ilk
    # güncellemede kendi sürüm dizinine kopyalandı (look-setup) — site onu çalıştırıyor sayılır.
    if [ "$CUR_FCGI" = "$OPT/lk-fcgi" ] && [ -s "$OPT/conf/legacy-release" ]; then
        CUR_FCGI="$REL/$(tr -cd 'A-Za-z0-9.+-' < "$OPT/conf/legacy-release")/lk-fcgi"
    fi
fi
CHECK_RC=0
if [ -x "$LATEST_DIR/lk" ]; then
    CHECK_OUT="$(cd "$DOC_ROOT" && runuser -u "$RUN_USER" -- env LOOK_FILE_ROOT="$DOC_ROOT" $ENV_WRAP "$LATEST_DIR/lk" --check "$SCRIPT" 2>&1)" || CHECK_RC=$?
else
    # lk yoksa (bozuk paket) site denetlenemez: denetimden geçmemiş sayılır — çalışan site
    # çalıştığı sürümde kalır, yeni site kurulmaz. Koruma tam bir şey ters gittiğinde gevşemesin.
    CHECK_RC=127
    CHECK_OUT="LOOK $LATEST_REL has no lk ($LATEST_DIR/lk), so this site could not be checked against it; install the extension again"
fi
CHECK_OUT="$(printf '%s\n' "$CHECK_OUT" | head -50)"
CHECK_SHORT="$(printf '%s\n' "$CHECK_OUT" | grep -E '^(CHECK |LOOK )' | head -3 | tr '\n' ';' | sed 's/;$//')"
HELD=false
if [ "$CHECK_RC" = 0 ]; then
    BIN_DIR="$LATEST_DIR"
elif [ -n "$CUR_FCGI" ] && [ -x "$CUR_FCGI" ] && [ "$(dirname "$CUR_FCGI")" != "$LATEST_DIR" ]; then
    BIN_DIR="$(dirname "$CUR_FCGI")"
    HELD=true
    echo "WARNING: $DOMAIN stays on LOOK $(rel_name "$BIN_DIR") and receives no fixes, security fixes included: it does not pass the check of LOOK $LATEST_REL ($CHECK_SHORT)"
elif [ "$CHECK_RC" = 127 ] && [ -n "$CUR_FCGI" ] && [ "$(dirname "$CUR_FCGI")" = "$LATEST_DIR" ]; then
    # Zaten en yeni sürümde (yeniden başlatma): taşınan bir şey yok — çalışmaya devam eder
    BIN_DIR="$LATEST_DIR"
    echo "WARNING: $CHECK_OUT"
else
    echo "ERROR: $DOMAIN: the code does not pass the check of LOOK $LATEST_REL; fix these lines and try again:" >&2
    printf '%s\n' "$CHECK_OUT" >&2
    exit 3   # look-setup: servise dokunma (çalışan site düşmesin), yalnız bildir
fi
mkdir -p "$STATUS_DIR"; chmod 755 "$OPT/conf" "$STATUS_DIR"
printf '{\n  "domain": "%s",\n  "release": "%s",\n  "latest": "%s",\n  "held": %s,\n  "check": "%s"\n}\n' \
    "$DOMAIN" "$(rel_name "$BIN_DIR")" "$LATEST_REL" "$HELD" "$( [ "$HELD" = true ] && json_str "$CHECK_OUT" )" \
    > "$STATUS_DIR/.$SVC_NAME.json.new" && chmod 644 "$STATUS_DIR/.$SVC_NAME.json.new" \
    && mv -f "$STATUS_DIR/.$SVC_NAME.json.new" "$STATUS_DIR/$SVC_NAME.json"

if [ "$MODE" = "http" ]; then
    # Yalniz yerel adreste dinler: Apache vekili 127.0.0.1:$PORT'a gider (asagida). Eskiden tum
    # arayuzlerde dinliyordu — site kendi portunda internete de acikti, Apache atlanabiliyordu.
    EXEC_CMD="$BIN_DIR/lk-fcgi --mode http --bind 127.0.0.1 --port $PORT --workers $WORKERS --script $SCRIPT"
else
    EXEC_CMD="$BIN_DIR/lk-fcgi --port $PORT --workers $WORKERS --script $SCRIPT"
fi
EXEC_CMD="${ENV_WRAP:+$ENV_WRAP }$EXEC_CMD"

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
${SESSION_ENV}

[Install]
WantedBy=multi-user.target
SYSTEMD

/bin/systemctl daemon-reload
/bin/systemctl enable "$SVC_NAME" 2>/dev/null || true
/bin/systemctl restart "$SVC_NAME"

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
    PROXY_CONF="<FilesMatch \"^\.look\">
    Require all denied
</FilesMatch>
ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / http://127.0.0.1:${PORT}/
ProxyPassReverse / http://127.0.0.1:${PORT}/"
else
    # FastCGI modu (ProxyPass ile — mod_proxy_fcgi)
    PROXY_CONF="<FilesMatch \"^\.look\">
    Require all denied
</FilesMatch>
ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / fcgi://127.0.0.1:${PORT}/
ProxyPassReverse / fcgi://127.0.0.1:${PORT}/"
fi

# X-Forwarded-Proto Apache tarafından YAZILIR (eklenmez): mod_proxy bunu kendiliğinden koymaz, istemcinin
# gönderdiği aynen geçerdi — LOOK HTTP isteğini HTTPS sanıp (oturum çerezi Secure, request::scheme)
# ona göre davranırdı. X-Forwarded-For / X-Forwarded-Host'u mod_proxy ekler; LOOK en sağdakini okur.
# IfModule: mod_headers yoksa yapılandırma bozulmasın (site düşmesin).
XFP_HTTP="<IfModule mod_headers.c>
    RequestHeader set X-Forwarded-Proto \"http\"
</IfModule>"
XFP_HTTPS="<IfModule mod_headers.c>
    RequestHeader set X-Forwarded-Proto \"https\"
</IfModule>"
printf '%s\n%s\n' "$PROXY_CONF" "$XFP_HTTP"  > "$CONF_DIR/vhost.conf"
printf '%s\n%s\n' "$PROXY_CONF" "$XFP_HTTPS" > "$CONF_DIR/vhost_ssl.conf"

# Apache reconfigure (Plesk yolu) — ARKA PLANDA çalışır. Plesk reconfigure 10-30sn
# sürer; senkron çalışırsa panel AJAX'ı kilitler/timeout olur (domain görünmez).
# systemd servisi + vhost config zaten hazır → reconfigure birkaç saniye içinde
# uygular. Hemen OK dönüyoruz, UI anında domain'i listeye ekler.
nohup bash -c '
    if [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "'"$DOMAIN"'"
    elif [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="'"$DOMAIN"'"
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

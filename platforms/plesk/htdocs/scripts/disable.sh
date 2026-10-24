#!/bin/bash
# LOOK Language — Domain devre disi birak
# Kullanim: disable.sh <svc_name> [<domain>] [remove]
# 'remove' (panelde domain kaldirma): unit dosyasini ve <svc>.service.d klasorunu da siler;
# yoksa (Stop) unit kalir, Start ile yeniden baslatilabilir.

set -e

SVC_NAME="$1"
DOMAIN="$2"
REMOVE="$3"

if [ -z "$SVC_NAME" ]; then
    echo "Kullanim: $0 <svc_name> [domain]" >&2
    exit 1
fi

# GUVENLIK: SVC_NAME sudo systemctl stop/disable'a gidiyor. look-* kisitlamasi olmadan
# panel kullanicisi ARBITRARY servisi (sshd, mariadb) root olarak durdurabilir (cok-kiracili
# DoS). look-* zorunlu + gecerli unit karakterlerine daralt. DOMAIN de dosya yolu/rm'e giriyor.
SVC_NAME=$(printf '%s' "$SVC_NAME" | tr -cd 'A-Za-z0-9.-')
DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
case "$SVC_NAME" in
    look-*) ;;
    *) echo "ERROR: only look-* services allowed" >&2; exit 1 ;;
esac

sudo /bin/systemctl stop "$SVC_NAME" 2>/dev/null || true
sudo /bin/systemctl disable "$SVC_NAME" 2>/dev/null || true

if [ "$REMOVE" = "remove" ]; then
    rm -f "/etc/systemd/system/${SVC_NAME}.service"
    rm -rf "/etc/systemd/system/${SVC_NAME}.service.d"
    sudo /bin/systemctl daemon-reload
fi

# vhost config temizle (Apache proxy'yi kaldir)
if [ -n "$DOMAIN" ]; then
    CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
    for f in vhost.conf vhost_ssl.conf; do
        [ -f "$CONF_DIR/$f" ] && rm -f "$CONF_DIR/$f"
    done

    # websrvmng Plesk Obsidian'da yok; httpdmng olmadan vhost.conf silinse de Apache eski proxy'yi tutar
    if [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        sudo /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "$DOMAIN" >/dev/null 2>&1 || true
    elif [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        sudo /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="$DOMAIN" 2>/dev/null || true
    fi
fi

echo "OK:stopped:${SVC_NAME}"

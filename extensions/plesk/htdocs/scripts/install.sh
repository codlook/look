#!/bin/bash
# LOOK Language — Kurulum scripti (Plesk root olarak calistirir)

BINARY_DIR="/opt/look"
LOG_DIR="/var/log/look"
CONF_DIR="/opt/look/conf"

# Scriptin calistigi dizin (Plesk'in temp extraction yolu)
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Buradan bin/ klasoru: htdocs/scripts/../bin = htdocs/bin
BIN_SRC="$(dirname "$SELF_DIR")/bin"

# Plesk kurulum sonrasi htdocs'un kopyalanacagi yol
HTDOCS_DIR="/usr/local/psa/admin/htdocs/modules/look-lang"
SCRIPTS_DIR="$HTDOCS_DIR/scripts"

echo "[LOOK] Kurulum basliyor... (script: $SELF_DIR)"
echo "[LOOK] Binary kaynak: $BIN_SRC"

mkdir -p "$BINARY_DIR" "$LOG_DIR" "$CONF_DIR"
chmod 755 "$BINARY_DIR"
chown psaadm:psaadm "$CONF_DIR" 2>/dev/null || true
chmod 755 "$CONF_DIR"

# 1. SUDOERS — en once yaz (root olarak calisiyoruz, sudo gerekmez)
cat > /etc/sudoers.d/look-lang << SUDOEOF
psaadm ALL=(root) NOPASSWD: ${SCRIPTS_DIR}/enable.sh, ${SCRIPTS_DIR}/disable.sh, ${SCRIPTS_DIR}/status.sh, /bin/systemctl start look-*, /bin/systemctl stop look-*, /bin/systemctl restart look-*, /bin/systemctl daemon-reload
SUDOEOF
chmod 440 /etc/sudoers.d/look-lang
echo "[LOOK] sudoers yazildi: /etc/sudoers.d/look-lang"

# 1b. Script izinleri — Plesk ZIP extract ettiginde +x biti kaybolur
chmod +x "$SCRIPTS_DIR/enable.sh" "$SCRIPTS_DIR/disable.sh" "$SCRIPTS_DIR/status.sh" "$SCRIPTS_DIR/install.sh" "$SCRIPTS_DIR/uninstall.sh" 2>/dev/null || true
echo "[LOOK] Script izinleri verildi"

# 2. Log dizini (psaadm yazabilsin)
mkdir -p /var/log/look
chmod 777 /var/log/look

# 3. PATH
echo 'export PATH="/opt/look:$PATH"' > /etc/profile.d/look.sh

# 4. Log rotasyon
cat > /etc/logrotate.d/look << 'ROTEOF'
/var/log/look/*.log {
    daily
    rotate 14
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
ROTEOF

# 5. BINARY KURULUM
install_binary() {
    # 5a. Scriptle ayni temp dizindeki bin/ klasoru (en guvenli)
    if [ -f "$BIN_SRC/lk-fcgi" ]; then
        echo "[LOOK] Bundled binary bulundu: $BIN_SRC"
        cp "$BIN_SRC/lk-fcgi" "$BINARY_DIR/lk-fcgi"
        chmod +x "$BINARY_DIR/lk-fcgi"
        [ -f "$BIN_SRC/lk" ]     && cp "$BIN_SRC/lk"     "$BINARY_DIR/lk"     && chmod +x "$BINARY_DIR/lk"
        [ -f "$BIN_SRC/lk-cgi" ] && cp "$BIN_SRC/lk-cgi" "$BINARY_DIR/lk-cgi" && chmod +x "$BINARY_DIR/lk-cgi"
        echo "[LOOK] Binary kopyalandi: $BINARY_DIR/lk-fcgi"
        return 0
    fi

    # 5b. Htdocs kopyalandiktan sonra kontrol (Plesk bazen once htdocs kopyalar)
    if [ -f "$HTDOCS_DIR/bin/lk-fcgi" ]; then
        echo "[LOOK] Htdocs/bin binary bulundu"
        cp "$HTDOCS_DIR/bin/lk-fcgi" "$BINARY_DIR/lk-fcgi"
        chmod +x "$BINARY_DIR/lk-fcgi"
        [ -f "$HTDOCS_DIR/bin/lk" ]     && cp "$HTDOCS_DIR/bin/lk"     "$BINARY_DIR/lk"     && chmod +x "$BINARY_DIR/lk"
        [ -f "$HTDOCS_DIR/bin/lk-cgi" ] && cp "$HTDOCS_DIR/bin/lk-cgi" "$BINARY_DIR/lk-cgi" && chmod +x "$BINARY_DIR/lk-cgi"
        return 0
    fi

    # 5c. Binary zaten varsa atla
    if [ -f "$BINARY_DIR/lk-fcgi" ]; then
        echo "[LOOK] Binary zaten mevcut"
        return 0
    fi

    # 5d. GitHub'dan indir
    ARCH=$(uname -m)
    GLIBC_MINOR=$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$' | cut -d. -f2 || echo "17")

    if [ "$ARCH" = "x86_64" ]; then
        TARBALL=$( [ "${GLIBC_MINOR:-17}" -lt 28 ] && echo "lk-linux-x64-el8.tar.gz" || echo "lk-linux-x64.tar.gz" )
    elif [ "$ARCH" = "aarch64" ]; then
        TARBALL="lk-linux-arm64.tar.gz"
    else
        echo "[UYARI] Desteklenmeyen mimari: $ARCH"
        return 1
    fi

    TMP=$(mktemp -d)
    RELEASE_URL="https://github.com/Codlook/look/releases/latest/download/$TARBALL"
    echo "[LOOK] Indiriliyor: $RELEASE_URL"

    if curl -fsSL --connect-timeout 15 "$RELEASE_URL" -o "$TMP/look.tar.gz" 2>/dev/null; then
        tar -xzf "$TMP/look.tar.gz" -C "$TMP" 2>/dev/null || true
        for BIN in lk-fcgi lk-cgi lk; do
            [ -f "$TMP/$BIN" ] && cp "$TMP/$BIN" "$BINARY_DIR/$BIN" && chmod +x "$BINARY_DIR/$BIN"
        done
        rm -rf "$TMP"
        return 0
    fi

    rm -rf "$TMP"
    echo "[UYARI] Binary kurulamadi — Ayarlar > Binary Guncelle ile manuel yukle"
    return 1
}

install_binary

# Versiyon
if [ -f "$BINARY_DIR/lk-fcgi" ]; then
    VERSION=$("$BINARY_DIR/lk-fcgi" version 2>/dev/null || echo "?")
    echo "[LOOK] Versiyon: $VERSION"
else
    echo "[UYARI] lk-fcgi bulunamadi — eklenti yuklendi ama binary eksik"
fi

# 6. Ikon — htdocs kopyalandiktan sonra (arka planda)
(sleep 5 && for SIZE in 32 64 128; do
    ICON_DIR="/usr/local/psa/admin/htdocs/modules/catalog/icons/$SIZE"
    ICON_SRC="$HTDOCS_DIR/icon.png"
    [ -f "$ICON_SRC" ] && [ -d "$ICON_DIR" ] && cp "$ICON_SRC" "$ICON_DIR/look-lang.png"
done) &

echo "[LOOK] Kurulum tamamlandi"

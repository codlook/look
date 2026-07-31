#!/bin/bash
# LOOK Language — kurulum (binary + sudoers + PATH). plib/scripts/post-install.php
# bunu ROOT olarak calistirir (Plesk extension install hook'u). Ayrica idempotent:
# eksik kurulumda controller da tetikleyebilir. Kök-dizindeki eski shell `post-install`
# Plesk tarafindan CALISTIRILMIYORDU (Plesk yalniz scripts/post-install.php kosar) —
# bu yuzden sudoers olusmuyor, `sudo` parola isteyip enable patliyordu.
set -e

INSTALL_DIR="/usr/local/psa/admin/htdocs/modules/look-lang"
OPT_DIR="/opt/look"
SCRIPTS_DIR="$INSTALL_DIR/scripts"
RPM_FILE="$INSTALL_DIR/look-lang.rpm"

mkdir -p "$OPT_DIR/conf" /var/log/look

install_via_rpm() {
    local mgr=""
    command -v dnf >/dev/null 2>&1 && mgr=dnf
    [ -z "$mgr" ] && command -v yum >/dev/null 2>&1 && mgr=yum
    [ -z "$mgr" ] && return 1
    [ -f "$RPM_FILE" ] || return 1
    echo "LOOK: RPM ile kuruluyor ($mgr)"
    "$mgr" install -y "$RPM_FILE" >/dev/null 2>&1 || rpm -Uvh --force "$RPM_FILE" >/dev/null 2>&1 || return 1
    for bin in lk lk-fcgi lk-cgi; do
        [ -x "/usr/bin/$bin" ] && ln -sf "/usr/bin/$bin" "$OPT_DIR/$bin"
    done
    return 0
}

install_bundled_static() {
    echo "LOOK: portatif static binary kuruluyor (fallback)"
    for bin in lk lk-fcgi lk-cgi; do
        if [ -f "$INSTALL_DIR/bin/$bin" ]; then
            cp "$INSTALL_DIR/bin/$bin" "$OPT_DIR/$bin"
            chmod +x "$OPT_DIR/$bin"
        fi
    done
}

install_via_rpm || install_bundled_static

chmod +x "$SCRIPTS_DIR"/*.sh 2>/dev/null || true

# Sudoers — psaadm ile scriptleri + servisleri parolasiz calistir (enable/disable
# controller'dan `sudo` ile cagrilir; bu kural OLMAZSA "a password is required" hatasi).
mkdir -p /etc/sudoers.d
cat > /etc/sudoers.d/look-lang << 'SUDO'
psaadm ALL=(root) NOPASSWD: \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/enable.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/disable.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/status.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/logs.sh *, \
  /bin/bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/monitor.sh *, \
  /bin/systemctl start look-*, \
  /bin/systemctl stop look-*, \
  /bin/systemctl restart look-*, \
  /bin/systemctl enable look-*, \
  /bin/systemctl disable look-*, \
  /bin/systemctl daemon-reload, \
  /usr/local/psa/admin/sbin/websrvmng, \
  /usr/local/psa/admin/bin/httpdmng
SUDO
chmod 440 /etc/sudoers.d/look-lang

echo 'export PATH="/opt/look:$PATH"' > /etc/profile.d/look.sh
[ -f "$OPT_DIR/conf/domains.json" ] || echo "[]" > "$OPT_DIR/conf/domains.json"

echo "LOOK Language kurulum tamam ($("$OPT_DIR/lk" --version 2>/dev/null || echo v1.1.0))."

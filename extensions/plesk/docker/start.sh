#!/bin/bash
set -e

echo "=== LOOK Plesk Extension Test Ortami ==="

# Script izinleri (her başlangıçta garanti et)
chmod +x /usr/local/psa/admin/htdocs/modules/look-lang/scripts/*.sh

# install.sh sonucunu göster
echo ""
echo "--- install.sh sonucu ---"
cat /tmp/install.log
echo "-------------------------"
echo ""

# Binary durumu
if [ -f /opt/look/lk-fcgi ]; then
    echo "[OK] Binary: /opt/look/lk-fcgi"
else
    echo "[WARN] Binary yok - htdocs/bin'den kopyalanıyor..."
    cp /usr/local/psa/admin/htdocs/modules/look-lang/bin/lk-fcgi /opt/look/lk-fcgi 2>/dev/null && \
    chmod +x /opt/look/lk-fcgi && echo "[OK] Binary kopyalandı" || echo "[ERR] Binary kopyalanamadı"
fi

# sudoers durumu
if [ -f /etc/sudoers.d/look-lang ]; then
    echo "[OK] sudoers: /etc/sudoers.d/look-lang"
else
    echo "[WARN] sudoers yok"
fi

# Log dizini izni
chmod 777 /var/log/look 2>/dev/null || true
chown -R psaadm:psaadm /var/log/look 2>/dev/null || true

# PHP için gerekli dizinler
chown -R psaadm:psaadm /usr/local/psa/admin/htdocs/modules/look-lang/ 2>/dev/null || true

echo ""
echo "=== Servisler başlatılıyor ==="
echo "URL: http://localhost:8880/modules/look-lang/index.php"
echo ""

# Apache foreground (mod_php ile)
exec /usr/sbin/httpd -D FOREGROUND

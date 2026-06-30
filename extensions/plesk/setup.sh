#!/bin/bash
# LOOK Language Plesk Extension Kurulum Scripti
# Kullanim: unzip -o look-lang.zip -d look-lang && sed -i "s/\r//" look-lang/setup.sh && bash look-lang/setup.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HTDOCS_DST="/usr/local/psa/admin/htdocs/modules/look-lang"

echo "[LOOK] Plesk eklentisi kuruluyor..."

# 1. Plesk'e kaydet
TMP_ZIP="/tmp/look-lang-install.zip"
ORIG_ZIP="$(dirname "$SCRIPT_DIR")/look-lang.zip"
plesk bin extension --uninstall look-lang 2>/dev/null || true
sleep 2
if [ -f "$ORIG_ZIP" ]; then
    # Orijinal indirilen ZIP varsa direkt kullan (zip kurulu olmasa da calisir)
    plesk bin extension -i "$ORIG_ZIP" 2>&1 | grep -v "^$" || true
elif command -v zip &>/dev/null; then
    cd "$SCRIPT_DIR" && zip -qr "$TMP_ZIP" . && cd - > /dev/null
    plesk bin extension -i "$TMP_ZIP" 2>&1 | grep -v "^$" || true
    rm -f "$TMP_ZIP"
elif command -v python3 &>/dev/null; then
    python3 -c "
import zipfile, os
src = '$SCRIPT_DIR'
with zipfile.ZipFile('$TMP_ZIP', 'w', zipfile.ZIP_DEFLATED) as z:
    for r, d, files in os.walk(src):
        for f in files:
            fp = os.path.join(r, f)
            z.write(fp, fp[len(src)+1:])
"
    plesk bin extension -i "$TMP_ZIP" 2>&1 | grep -v "^$" || true
    rm -f "$TMP_ZIP"
else
    echo "[LOOK] HATA: zip/python3 bulunamadi ve orijinal ZIP yok"
    echo "[LOOK] Cozum: zip yukle (yum install zip) veya look-lang.zip dosyasini koru"
    exit 1
fi

# 2. htdocs'u dogru yere kopyala
mkdir -p "$HTDOCS_DST"
cp -r "$SCRIPT_DIR/htdocs/." "$HTDOCS_DST/"

# 3. CRLF temizle ve izinleri ayarla
find "$HTDOCS_DST/scripts" -name "*.sh" -exec sed -i "s/\r//" {} \;
chmod +x "$HTDOCS_DST/scripts/"*.sh
chmod +x "$HTDOCS_DST/bin/"* 2>/dev/null || true

# 4. Kurulum scriptini calistir
bash "$HTDOCS_DST/scripts/install.sh"

echo "[LOOK] Panel: https://$(hostname -f 2>/dev/null || echo '<plesk-ip>')/modules/look-lang/"

#!/bin/bash
# LOOK — Linux standalone (Ubuntu/Debian + AlmaLinux/RHEL) paketi olustur.
# Çalıştırma: repo kökünden `bash platforms/linux/build.sh`
#
# Gömülü binary = PORTATİF statik build (cpp/build-portable): statik OpenSSL/libstdc++,
# glibc 2.28 → glibc>=2.28 tüm dağıtımlarda + gelecek sürümlerde çalışır. Önce üret:
#   docker run --rm -v "$PWD/cpp:/look/cpp" -w /look/cpp look-build bash build-portable.sh
set -e
cd "$(dirname "$0")"

VERSION="1.0.0"
OUT="look-lang-linux-${VERSION}.zip"
PORT="../../cpp/build-portable"
TMP="$(mktemp -d)"

for b in lk lk-fcgi; do
    [ -f "$PORT/$b" ] || { echo "HATA: portatif binary yok ($PORT/$b) — önce build-portable.sh" >&2; exit 1; }
done

echo "Building LOOK Linux package v$VERSION..."
mkdir -p "$TMP/bin"
cp ubuntu/install.sh ubuntu/start.sh ubuntu/README.md "$TMP/"
chmod +x "$TMP/install.sh" "$TMP/start.sh"
for b in lk lk-fcgi; do
    cp "$PORT/$b" "$TMP/bin/$b"
    chmod +x "$TMP/bin/$b"
done

# Portatiflik teyidi — dinamik ssl/crypto/stdc++ olmamalı
if ldd "$TMP/bin/lk-fcgi" 2>/dev/null | grep -qiE 'ssl|crypto|stdc\+\+'; then
    echo "UYARI: bin/lk-fcgi dinamik ssl/crypto/stdc++ bağlı — portatif DEĞİL!" >&2; exit 1
fi

(cd "$TMP" && zip -r "$(pwd)/$OUT" . >/dev/null)
mv "$TMP/$OUT" "./$OUT"
rm -rf "$TMP"
echo "OK: platforms/linux/$OUT"
ls -lh "./$OUT"

#!/bin/bash
# LOOK Language Plesk Extension — ZIP paketi olustur
# Kullanim: ./build.sh [--with-binaries]
# Calistirma: repo kökünden: bash platforms/plesk/build.sh

set -e
cd "$(dirname "$0")"

VERSION="1.0.0"
OUT="look-lang-plesk-${VERSION}.zip"
TMP="$(mktemp -d)"

echo "Building LOOK Plesk Extension v$VERSION..."

# Temel dosyalar
cp meta.xml     "$TMP/"
cp post-install "$TMP/"
cp pre-uninstall "$TMP/"
chmod +x "$TMP/post-install" "$TMP/pre-uninstall"

# plib — Plesk Obsidian MVC controller (Extensions paneli buradan dispatch eder)
if [ -d plib ]; then
    mkdir -p "$TMP/plib/controllers"
    cp -r plib/* "$TMP/plib/"
fi

# htdocs
mkdir -p "$TMP/htdocs/scripts" "$TMP/htdocs/bin"
cp htdocs/index.php "$TMP/htdocs/"

# UI: phtml view + derlenmiş frontend (bunlar olmadan panel arayüzü render olmaz)
if [ -d htdocs/phtml ]; then
    mkdir -p "$TMP/htdocs/phtml"
    cp htdocs/phtml/*.phtml "$TMP/htdocs/phtml/" 2>/dev/null || true
fi
if [ -d htdocs/dist ]; then
    mkdir -p "$TMP/htdocs/dist"
    cp htdocs/dist/* "$TMP/htdocs/dist/" 2>/dev/null || true
fi

for sh in htdocs/scripts/*.sh; do
    cp "$sh" "$TMP/htdocs/scripts/"
    chmod +x "$TMP/htdocs/scripts/$(basename $sh)"
done

# Icon
if [ -f "htdocs/icon.png" ]; then
    cp htdocs/icon.png "$TMP/htdocs/"
fi

# Portatif static binary'ler — fallback (Debian-Plesk / RPM olmayan sistemler)
if [ "$1" = "--with-binaries" ] || [ -d "bin" ]; then
    for bin in lk lk-fcgi lk-cgi; do
        if [ -f "bin/$bin" ]; then
            cp "bin/$bin" "$TMP/htdocs/bin/"
            chmod +x "$TMP/htdocs/bin/$bin"
        fi
    done
fi

cp ../../cpp/tests/release_gate.lk "$TMP/htdocs/"   # yayin kapisi: htdocs/bin/lk htdocs/release_gate.lk

# RPM — RHEL/AlmaLinux birincil yol (dnf update look-lang). post-install önce
# bunu dener; yoksa yukarıdaki static binary'ye düşer.
RPM_SRC="$(ls ../linux/rpm/out/look-lang-${VERSION}*.rpm 2>/dev/null | head -1)"
if [ -n "$RPM_SRC" ] && [ -f "$RPM_SRC" ]; then
    cp "$RPM_SRC" "$TMP/htdocs/look-lang.rpm"
    echo "  + RPM dahil edildi: $(basename "$RPM_SRC")"
else
    echo "  ! RPM bulunamadı (önce: bash platforms/linux/rpm/build-rpm.sh) — sadece static binary"
fi

# ZIP olustur
(cd "$TMP" && zip -r "$(pwd)/$OUT" .)
mv "$TMP/$OUT" "./$OUT"
rm -rf "$TMP"

echo "OK: platforms/plesk/$OUT"
ls -lh "./$OUT"

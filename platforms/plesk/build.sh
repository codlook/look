#!/bin/bash
# LOOK Language Plesk Extension — build the ZIP package
# Usage: ./build.sh [--with-binaries]
# Run: from the repo root: bash platforms/plesk/build.sh

set -e
cd "$(dirname "$0")"

VERSION="$(bash ../version.sh)"
OUT="look-lang-plesk-${VERSION}.zip"
TMP="$(mktemp -d)"

echo "Building LOOK Plesk Extension v$VERSION..."

# Core files
# (The hooks are in plib/scripts/post-install.php; the old post-install/pre-uninstall in the root were never run by Plesk
# and carried the old flow that wrote sudoers — removed.)
cp meta.xml     "$TMP/"

# plib — Plesk Obsidian MVC controller (the Extensions panel dispatches from here)
if [ -d plib ]; then
    mkdir -p "$TMP/plib/controllers"
    cp -r plib/* "$TMP/plib/"
fi

# sbin — the root step (binary, sudoers, service units). post-install.php calls this
# with pm_ApiCli::callSbin; Plesk hooks run as psaadm, not root.
mkdir -p "$TMP/sbin"
cp sbin/look-setup sbin/look-ctl "$TMP/sbin/"
chmod 755 "$TMP/sbin/look-setup" "$TMP/sbin/look-ctl"

# htdocs
mkdir -p "$TMP/htdocs/scripts" "$TMP/htdocs/bin"
cp htdocs/index.php "$TMP/htdocs/"

# UI: phtml view + compiled frontend (without these the panel UI does not render)
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

# Portable static binaries — fallback (Debian-Plesk / systems without RPM)
if [ "$1" = "--with-binaries" ] || [ -d "bin" ]; then
    for bin in lk lk-fcgi lk-cgi; do
        if [ -f "bin/$bin" ]; then
            cp "bin/$bin" "$TMP/htdocs/bin/"
            chmod +x "$TMP/htdocs/bin/$bin"
        fi
    done
fi


# RPM — the primary path for RHEL/AlmaLinux (dnf update look-lang). post-install tries
# this first; if missing it falls back to the static binary above.
# Latest release of the maintenance line (e.g. bin-1.0/): a site whose code fails the check of the newest release is moved to the newest release
# that passes (sbin/look-setup + scripts/enable.sh) — the release workflow downloads and places it.
for dir in bin-*/; do
    [ -f "$dir/lk-fcgi" ] || continue
    mkdir -p "$TMP/htdocs/$dir"
    for bin in lk lk-fcgi lk-cgi; do
        [ -f "$dir/$bin" ] && { cp "$dir/$bin" "$TMP/htdocs/$dir"; chmod +x "$TMP/htdocs/$dir/$bin"; }
    done
    echo "  + ${dir%/}: $("$dir/lk" --version 2>/dev/null | head -1)"
done
RPM_SRC="$(ls ../linux/rpm/out/look-lang-${VERSION}*.rpm 2>/dev/null | head -1)"
if [ -n "$RPM_SRC" ] && [ -f "$RPM_SRC" ]; then
    cp "$RPM_SRC" "$TMP/htdocs/look-lang.rpm"
    echo "  + RPM included: $(basename "$RPM_SRC")"
else
    echo "  ! no RPM found (build it first: bash platforms/linux/rpm/build-rpm.sh) — static binary only"
fi

# Build the ZIP
(cd "$TMP" && zip -r "$(pwd)/$OUT" .)
mv "$TMP/$OUT" "./$OUT"
rm -rf "$TMP"

echo "OK: platforms/plesk/$OUT"
ls -lh "./$OUT"

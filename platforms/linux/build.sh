#!/bin/bash
# LOOK — build the Linux standalone package (Ubuntu/Debian + AlmaLinux/RHEL).
# Run from the repo root: `bash platforms/linux/build.sh`
#
# Embedded binary = PORTABLE static build (cpp/build-portable): static OpenSSL/libstdc++,
# glibc 2.28 → works on every distribution with glibc>=2.28 and on future releases. Build it first:
#   docker run --rm -v "$PWD/cpp:/look/cpp" -w /look/cpp look-build bash build-portable.sh
set -e
cd "$(dirname "$0")"

VERSION="$(bash ../version.sh)"
OUT="look-lang-linux-${VERSION}.zip"
PORT="../../cpp/build-portable"
TMP="$(mktemp -d)"

for b in lk lk-fcgi lk-cgi; do
    [ -f "$PORT/$b" ] || { echo "ERROR: portable binary missing ($PORT/$b) — run build-portable.sh first" >&2; exit 1; }
done

echo "Building LOOK Linux package v$VERSION..."
mkdir -p "$TMP/bin"
cp ubuntu/install.sh ubuntu/start.sh ubuntu/README.md "$TMP/"
chmod +x "$TMP/install.sh" "$TMP/start.sh"
for b in lk lk-fcgi lk-cgi; do
    cp "$PORT/$b" "$TMP/bin/$b"
    chmod +x "$TMP/bin/$b"
done

# Portability check — there must be no dynamic ssl/crypto/stdc++
if ldd "$TMP/bin/lk-fcgi" 2>/dev/null | grep -qiE 'ssl|crypto|stdc\+\+'; then
    echo "WARNING: bin/lk-fcgi is dynamically linked to ssl/crypto/stdc++ — NOT portable!" >&2; exit 1
fi

(cd "$TMP" && zip -r "$(pwd)/$OUT" . >/dev/null)
mv "$TMP/$OUT" "./$OUT"
rm -rf "$TMP"
echo "OK: platforms/linux/$OUT"
ls -lh "./$OUT"

#!/usr/bin/env bash
# LOOK — AlmaLinux/RHEL RPM paketi üret (dnf install/update look-lang).
#
# Binary DİNAMİK OpenSSL'e bağlanır (kurumsal doğru yol): 'dnf update openssl-libs'
# güvenlik yamasını bize de yansıtır, crypto-policies/FIPS'e uyulur. libstdc++/gcc
# STATİK linklenir (gcc-toolset-12 binary'si stok AlmaLinux 8 libstdc++'ında
# GLIBCXX_3.4.29 eksikliği yaşamasın).
#
# Çalıştırma (repo kökünden, Docker):
#   docker run --rm -v "$PWD:/look" -w /look almalinux:8 bash platforms/linux/rpm/build-rpm.sh
set -euo pipefail
VERSION="${VERSION:-1.0.0}"
SPEC=/look/platforms/linux/rpm/look-lang.spec

dnf install -y --setopt=sslverify=false gcc-toolset-12 cmake make openssl-devel \
    rpm-build rpmdevtools > /tmp/dnf.log 2>&1
source /opt/rh/gcc-toolset-12/enable

# ── 1. DİNAMİK binary derle (sistem OpenSSL + statik libstdc++) ────────────────
cd /look/cpp
rm -rf build-rpm
cmake -S . -B build-rpm -DCMAKE_BUILD_TYPE=Release \
      -DLOOK_STATIC_SSL=OFF \
      -DLOOK_BUILD="${LOOK_BUILD:-src}" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" > /tmp/cmake.log 2>&1
cmake --build build-rpm --target look look-fcgi look-cgi -j8 > /tmp/build.log 2>&1
echo "[rpm] binary derlendi (dinamik OpenSSL):"
ldd build-rpm/lk-fcgi | grep -iE "ssl|crypto" | sed 's/^/    /' || echo "    (!! OpenSSL statik olmamalı)"

# ── 2. rpmbuild ağacı ─────────────────────────────────────────────────────────
RPMTOP=/root/rpmbuild
rpmdev-setuptree
cp build-rpm/lk build-rpm/lk-fcgi build-rpm/lk-cgi "$RPMTOP/SOURCES/"
cp "$SPEC" "$RPMTOP/SPECS/"

# ── 3. RPM üret ───────────────────────────────────────────────────────────────
rpmbuild -bb "$RPMTOP/SPECS/look-lang.spec" > /tmp/rpmbuild.log 2>&1 || { echo "RPMBUILD FAIL"; tail -20 /tmp/rpmbuild.log; exit 1; }
RPM=$(find "$RPMTOP/RPMS" -name "look-lang-${VERSION}*.rpm" | head -1)
mkdir -p /look/platforms/linux/rpm/out
cp "$RPM" /look/platforms/linux/rpm/out/
echo "[rpm] ÜRETİLDİ: platforms/linux/rpm/out/$(basename "$RPM")"
rpm -qpi "$RPM" 2>/dev/null | grep -iE "Name|Version|Size|Requires" | sed 's/^/    /'
echo "[rpm] bağımlılıklar:"; rpm -qpR "$RPM" 2>/dev/null | grep -iE "openssl|libssl|glibc|libc.so" | sed 's/^/    /'

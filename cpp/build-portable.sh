#!/usr/bin/env bash
# Portatif LOOK binary — statik OpenSSL + statik libstdc++/libgcc.
# Sonuc: libssl/libcrypto/libstdc++ bagimliligi YOK → AlmaLinux 8 (OpenSSL 1.1)
# VE AlmaLinux 9 / Ubuntu 22+ (OpenSSL 3.x) HEPSINDE calisir. Plugin/release binary'si.
#
# look-build imajinda kosar (statik OpenSSL /opt/openssl-static'te gomulu — Dockerfile.build):
#   docker run --rm -v "$PWD/cpp:/look/cpp" -w /look/cpp look-build bash build-portable.sh
set -e
source /opt/rh/gcc-toolset-12/enable 2>/dev/null || true
S="${OPENSSL_STATIC_ROOT:-/opt/openssl-static}"
cd "$(dirname "$0")"

if [ ! -f "$S/lib/libssl.a" ]; then
  echo "HATA: statik OpenSSL bulunamadi ($S). look-build imajini guncelle:" >&2
  echo "  docker build -t look-build -f cpp/Dockerfile.build cpp/" >&2
  exit 1
fi

rm -rf build-portable
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DLOOK_STATIC_SSL=ON \
  -DLOOK_BUILD="${LOOK_BUILD:-src}" \
  -DOPENSSL_INCLUDE_DIR="$S/include" \
  -DOPENSSL_SSL_LIBRARY="$S/lib/libssl.a" \
  -DOPENSSL_CRYPTO_LIBRARY="$S/lib/libcrypto.a" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-portable --target look look-fcgi look-cgi -j"$(nproc)"

echo "── Portatif binary'ler: cpp/build-portable/{lk,lk-fcgi,lk-cgi} ──"
if ldd build-portable/lk-fcgi 2>&1 | grep -qiE 'ssl|crypto|stdc\+\+'; then
  echo "UYARI: dinamik ssl/crypto/stdc++ bagimliligi var — portatif DEGIL!"; exit 1
else
  echo "OK: statik (yalniz glibc + libz) — tum dagitimlar."
fi

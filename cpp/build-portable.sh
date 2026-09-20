#!/usr/bin/env bash
set -e
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
# OpenSSL 3.5.x LTS (statik). Prefix sürüme-özel → sürüm bump'ı eski cache'i doğal
# olarak geçersiz kılar (eskiden sabit _ssl-static, bayat 1.1.1w'yi ship edebilirdi).
# --libdir=lib: OpenSSL 3.x x86_64'te lib64'e kurar; CMake yolları lib/ bekliyor → sabitle.
OSSL_VER=3.5.8
OSSL_SHA=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
S=/look/cpp/_ssl-static-$OSSL_VER
if [ ! -f "$S/lib/libcrypto.a" ]; then
  command -v perl >/dev/null || dnf install -y --setopt=sslverify=false perl-core >/dev/null 2>&1
  cd /tmp
  if [ ! -d "openssl-$OSSL_VER" ]; then
    curl -sL -o o.tgz "https://github.com/openssl/openssl/releases/download/openssl-$OSSL_VER/openssl-$OSSL_VER.tar.gz"
    echo "$OSSL_SHA  o.tgz" | sha256sum -c - || { echo "OpenSSL tarball SHA256 MISMATCH — aborting"; exit 1; }
    tar xzf o.tgz
  fi
  cd "openssl-$OSSL_VER"
  ./config no-shared no-tests no-zlib --libdir=lib --prefix="$S" >/dev/null 2>&1
  make -j8 >/dev/null 2>&1; make install_sw >/dev/null 2>&1
fi
cd /look/cpp; rm -rf build-portable
# LOOK_BUILD env varsa damgayı ONA sabitle (container'da .git yok → git rev-parse "src"e düşer,
# ama release "1.0.0 sabit, ayrım damgada" modeli git-sha ister). Env yoksa CMakeLists git/src'e düşer.
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DLOOK_STATIC_SSL=ON \
  ${LOOK_BUILD:+-DLOOK_BUILD="$LOOK_BUILD"} \
  -DOPENSSL_INCLUDE_DIR="$S/include" -DOPENSSL_SSL_LIBRARY="$S/lib/libssl.a" -DOPENSSL_CRYPTO_LIBRARY="$S/lib/libcrypto.a" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" >/tmp/cfg.log 2>&1 && echo "CONFIG OK" || { echo CFG_FAIL; tail -12 /tmp/cfg.log; exit 1; }
cmake --build build-portable --target look look-fcgi look-cgi -j8 2>&1 | grep -iE 'error:|Built target'
echo "=== ldd (glibc-only olmali) ==="; ldd build-portable/lk-fcgi 2>&1 | grep -iE 'ssl|crypto|stdc|=>'
echo "surum: $(build-portable/lk --version 2>&1 | head -1)"
printf '$a=9007199254740992+1\n$b=9007199254740992\nprint($a==$b)\n' > /tmp/pv.lk
echo -n "int64(false olmali): "; build-portable/lk /tmp/pv.lk 2>&1
echo "PORTABLE_DONE"

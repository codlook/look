#!/usr/bin/env bash
set +e
dnf install -y --setopt=sslverify=false gcc-toolset-12-libtsan-devel >/dev/null 2>&1
source /opt/rh/gcc-toolset-12/enable 2>/dev/null
cd /look/cpp
[ -f build-tsan/CMakeCache.txt ] || cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOOK_SANITIZE=thread \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so -DOPENSSL_INCLUDE_DIR=/usr/include >/tmp/g.log 2>&1
cmake --build build-tsan --target look-fcgi -j8 >/tmp/b.log 2>&1 && echo "TSAN look-fcgi OK: $(ls -la build-tsan/lk-fcgi 2>/dev/null|awk '{print $5}')" || { echo BUILD_FAIL; tail -12 /tmp/b.log; }

#!/usr/bin/env bash
# closure_parallel/tsan_guard.sh — parallel task closure-capture deep-clone yarışı guard'ı.
#
# BULGU (58 capture+parallel ekseni): parallel task bir CLOSURE yakaladığında,
# PARALLEL_CALL deep-clone o closure'ı SIĞ kopyalıyordu (deep_clone_impl fn için *this)
# → closure-içi cell parent'la PAYLAŞIMLI → veri yarışı (np1). 2b'nin thread-safety'si
# bir seviye derinde delik veriyordu. FIX: deep-clone transitif — closure capture'ları da
# klonlanır (vm.cpp bc_fn_cloner hook + PARALLEL_CALL closure clone).
#
# POZİTİF KONTROL: -DLOOK_NO_TRANSITIVE_CLONE eski (yarışlı) hâli derler.
#   FIX (default):               np1 TSan TEMİZ, fonksiyonel izole (0)
#   BUG (-DLOOK_NO_TRANSITIVE_CLONE): np1 TSan YARIŞ (heap cell), fonksiyonel racy
# np2 (doğrudan cell) her iki hâlde izole (123) — kontrast.
#
# Kullanım (Docker, gcc-toolset-12 + libtsan):
#   dnf install -y gcc-toolset-12-libtsan-devel; source /opt/rh/gcc-toolset-12/enable
#   echo 0 > /proc/sys/kernel/randomize_va_space   # TSan ASLR FATAL gürültüsü için (privileged)
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLOOK_SANITIZE=thread \
#         -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so
#   cmake --build build-tsan --target look -j8
#   bash cpp/tests/closure_parallel/tsan_guard.sh build-tsan/lk
set -u
LK="${1:-build-tsan/lk}"
DIR="$(dirname "$0")"
fail=0
for i in 1 2 3 4 5 6 7 8; do
  o=$(TSAN_OPTIONS="halt_on_error=0" "$LK" "$DIR/np1.lk" 2>&1)
  if echo "$o" | grep -q "data race"; then
    echo "FAIL np1: closure-içi cell YARIŞI (transitif deep-clone eksik)"; echo "$o" | grep -iE "data race|heap block" | head -2
    fail=1; break
  fi
done
[ $fail = 0 ] && echo "PASS np1: closure-capture deep-clone transitif — yarış yok"
# fonksiyonel izolasyon (VM): np1=0 (izole), np2=123 (izole)
v1=$("$LK" "$DIR/np1.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
v2=$("$LK" "$DIR/np2.lk" 2>&1 | grep -v "INFO\|Pool" | tr -d '\n ')
[ "$v1" = "0" ] && echo "PASS np1 izole: 0" || { echo "FAIL np1 izole: [$v1] (0 beklenir)"; fail=1; }
[ "$v2" = "123" ] && echo "PASS np2 izole: 123" || { echo "FAIL np2 izole: [$v2] (123 beklenir)"; fail=1; }
exit $fail

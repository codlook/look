// int_overflow.h — int64 aritmetiği taşma-tespiti (SAF, tablo-test edilebilir).
// interpreter.cpp'nin operator+/-/* bunları çağırır; taşınca çağıran float'a promote eder
// (büyük-literal float davranışıyla tutarlı, signed-overflow UB YOK).
// Portatif: GCC/Clang __builtin_*_overflow; MSVC unsigned-sarma + bölme kontrolü.
#pragma once
#include <cstdint>

namespace look {

inline bool i64_add_ovf(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, r);
#else
    uint64_t ur = (uint64_t)a + (uint64_t)b; *r = (int64_t)ur;
    return ((a ^ *r) & (b ^ *r)) < 0;
#endif
}

inline bool i64_sub_ovf(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, r);
#else
    uint64_t ur = (uint64_t)a - (uint64_t)b; *r = (int64_t)ur;
    return ((a ^ b) & (a ^ *r)) < 0;
#endif
}

inline bool i64_mul_ovf(int64_t a, int64_t b, int64_t* r) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, r);
#else
    *r = (int64_t)((uint64_t)a * (uint64_t)b);
    // INT64_MIN / -1 matematiksel sonucu 2^63 → int64'e sığmaz → x86 idiv #DE (SIGFPE).
    // Bu guard'ı BÖLMEDEN ÖNCE koy: kısa-devreli || içinde bölmeden sonra gelirse, guard'a
    // ulaşmadan süreç çöker (-1 * INT64_MIN). Yalnız MSVC dalı — GCC/Clang __builtin kullanır.
    if (a == -1 && b == INT64_MIN) return true;
    if (a != 0 && *r / a != b)     return true;
    return false;
#endif
}

} // namespace look

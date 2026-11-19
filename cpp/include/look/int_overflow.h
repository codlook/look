// int_overflow.h -- overflow detection for int64 arithmetic (PURE, table-testable).
// operator+/-/* in value.cpp call these; on overflow the caller promotes to float
// (consistent with big-literal float behavior, no signed-overflow UB).
// Portable: GCC/Clang __builtin_*_overflow; MSVC unsigned wrap + division check.
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
    // INT64_MIN / -1: the mathematical result 2^63 does not fit int64 -> x86 idiv #DE (SIGFPE).
    // Put this guard BEFORE the division: if it comes after the division inside a short-circuit ||, the process
    // crashes before reaching the guard (-1 * INT64_MIN). MSVC branch only -- GCC/Clang use __builtin.
    if (a == -1 && b == INT64_MIN) return true;
    if (a != 0 && *r / a != b)     return true;
    return false;
#endif
}

} // namespace look

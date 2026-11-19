// int64 overflow-detection table test — look/int_overflow.h (open the seam).
// interpreter.cpp operator+/-/* call these; on overflow they promote to float. This test locks the overflow
// BOUNDARY with positive controls (Linux/CI __builtin branch; the MSVC branch is Tier-2 Windows).
//   Build: g++ -std=c++17 -Iinclude tests/int_overflow_test.cpp -o /tmp/iov && /tmp/iov
#include "look/int_overflow.h"
#include <cstdio>
#include <cstdint>
#include <climits>
using namespace look;

enum Op { ADD, SUB, MUL };
static int fails = 0, ran = 0;

// CALL the function (avoid the arg-evaluation-order trap: run first, then compare),
// verify the overflow flag and (if no overflow) the result.
static void chk(const char* label, Op op, int64_t a, int64_t b, bool want_ovf, int64_t want_r) {
    ran++;
    int64_t r = 0;
    bool ovf = (op == ADD) ? i64_add_ovf(a, b, &r)
             : (op == SUB) ? i64_sub_ovf(a, b, &r)
                           : i64_mul_ovf(a, b, &r);
    bool ok = (ovf == want_ovf) && (want_ovf || r == want_r);
    printf("  %-28s ovf=%d want=%d r=%lld %s\n", label, ovf, want_ovf,
           (long long)r, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("int64 overflow-detection table:\n");
    // ── addition ──
    chk("add 2+3",            ADD, 2, 3, false, 5);
    chk("add MAX+0",          ADD, INT64_MAX, 0, false, INT64_MAX);
    chk("add MAX+1 OVERFLOWS", ADD, INT64_MAX, 1, true, 0);
    chk("add MIN+(-1) OVERFLOWS", ADD, INT64_MIN, -1, true, 0);
    chk("add -5+(-3)",        ADD, -5, -3, false, -8);
    // ── subtraction ──
    chk("sub 10-4",           SUB, 10, 4, false, 6);
    chk("sub MIN-1 OVERFLOWS", SUB, INT64_MIN, 1, true, 0);
    chk("sub MAX-(-1) OVERFLOWS", SUB, INT64_MAX, -1, true, 0);
    chk("sub MIN-(-1)",       SUB, INT64_MIN, -1, false, INT64_MIN + 1);
    // ── multiplication ──
    chk("mul 6*7",            MUL, 6, 7, false, 42);
    chk("mul MAX*1",          MUL, INT64_MAX, 1, false, INT64_MAX);
    chk("mul MAX*2 OVERFLOWS", MUL, INT64_MAX, 2, true, 0);
    chk("mul MIN*-1 OVERFLOWS", MUL, INT64_MIN, -1, true, 0);   // #DE trap — the guard must hold
    chk("mul -1*MIN OVERFLOWS", MUL, -1, INT64_MIN, true, 0);
    chk("mul 0*MIN zero",    MUL, 0, INT64_MIN, false, 0);   // the division guard skips at a==0
    chk("mul MIN*0 zero",    MUL, INT64_MIN, 0, false, 0);
    chk("mul 9e18 below-limit",MUL, 3000000000LL, 3000000000LL, false, 9000000000000000000LL); // 9e18 < MAX
    chk("mul 4e9*4e9 OVERFLOWS", MUL, 4000000000LL, 4000000000LL, true, 0);  // 1.6e19 > MAX
    chk("mul -4*3",           MUL, -4, 3, false, -12);

    const int EXPECTED = 19;
    if (ran != EXPECTED) { printf("\nFAIL: %d cases expected, %d ran\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nALL CASES PASSED (MIN*-1 #DE trap and overflow boundaries locked in)\n", fails);
    return fails ? 1 : 0;
}

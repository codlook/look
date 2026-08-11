// int64 taşma-tespiti tablo testi — look/int_overflow.h (dikişi aç).
// interpreter.cpp operator+/-/* bunları çağırır; taşınca float promote. Bu test taşma
// SINIRINI POZİTİF-KONTROLLÜ kilitler (Linux/CI __builtin dalı; MSVC dalı Tier-2 Windows).
//   Build: g++ -std=c++17 -Iinclude tests/int_overflow_test.cpp -o /tmp/iov && /tmp/iov
#include "look/int_overflow.h"
#include <cstdio>
#include <cstdint>
#include <climits>
using namespace look;

enum Op { ADD, SUB, MUL };
static int fails = 0, ran = 0;

// Fonksiyonu ÇAĞIR (arg-eval sırası tuzağından kaçın: önce çalıştır, sonra karşılaştır),
// taşma-bayrağını ve (taşma yoksa) sonucu doğrula.
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
    printf("int64 taşma-tespiti tablosu:\n");
    // ── toplama ──
    chk("add 2+3",            ADD, 2, 3, false, 5);
    chk("add MAX+0",          ADD, INT64_MAX, 0, false, INT64_MAX);
    chk("add MAX+1 TAŞAR",    ADD, INT64_MAX, 1, true, 0);
    chk("add MIN+(-1) TAŞAR", ADD, INT64_MIN, -1, true, 0);
    chk("add -5+(-3)",        ADD, -5, -3, false, -8);
    // ── çıkarma ──
    chk("sub 10-4",           SUB, 10, 4, false, 6);
    chk("sub MIN-1 TAŞAR",    SUB, INT64_MIN, 1, true, 0);
    chk("sub MAX-(-1) TAŞAR", SUB, INT64_MAX, -1, true, 0);
    chk("sub MIN-(-1)",       SUB, INT64_MIN, -1, false, INT64_MIN + 1);
    // ── çarpma ──
    chk("mul 6*7",            MUL, 6, 7, false, 42);
    chk("mul MAX*1",          MUL, INT64_MAX, 1, false, INT64_MAX);
    chk("mul MAX*2 TAŞAR",    MUL, INT64_MAX, 2, true, 0);
    chk("mul MIN*-1 TAŞAR",   MUL, INT64_MIN, -1, true, 0);   // #DE tuzağı — guard tutmalı
    chk("mul -1*MIN TAŞAR",   MUL, -1, INT64_MIN, true, 0);
    chk("mul 0*MIN sıfır",    MUL, 0, INT64_MIN, false, 0);   // bölme-guard'ı a==0'da atlar
    chk("mul MIN*0 sıfır",    MUL, INT64_MIN, 0, false, 0);
    chk("mul 9e18 sınır-altı",MUL, 3000000000LL, 3000000000LL, false, 9000000000000000000LL); // 9e18 < MAX
    chk("mul 4e9*4e9 TAŞAR",  MUL, 4000000000LL, 4000000000LL, true, 0);  // 1.6e19 > MAX
    chk("mul -4*3",           MUL, -4, 3, false, -12);

    const int EXPECTED = 19;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (MIN*-1 #DE tuzağı ve taşma sınırları kilitli)\n", fails);
    return fails ? 1 : 0;
}

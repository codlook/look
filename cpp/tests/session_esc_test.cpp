// session:: SID doğrulama + blob enjeksiyon tablo testi — look/session_esc.h (dikişi aç).
// web_stdlib.cpp session:: bunları çağırır. İki güvenlik-kararı POZİTİF-KONTROLLÜ:
//   1) valid_sid — 32-hex allowlist, path-traversal reddi.
//   2) sess_blob_set/get — \n / = enjeksiyonu ile uydurma yetki-alanı ENJEKTE EDİLEMEZ.
//   Build: g++ -std=c++17 -Iinclude tests/session_esc_test.cpp -o /tmp/sess && /tmp/sess
#include "look/session_esc.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
static void chk(const char* label, bool cond) {
    ran++;
    printf("  %-42s %s\n", label, cond ? "OK" : "FAIL");
    if (!cond) fails++;
}

int main() {
    printf("session güvenlik-karar tablosu:\n");
    // ── valid_sid: 32-hex allowlist ──
    chk("gecerli 32-hex",       valid_sid("0123456789abcdef0123456789abcdef"));
    chk("BUYUK harf hex kabul",  valid_sid("0123456789ABCDEF0123456789ABCDEF"));
    chk("31 char RED",          !valid_sid("0123456789abcdef0123456789abcde"));
    chk("33 char RED",          !valid_sid("0123456789abcdef0123456789abcdef0"));
    chk("bos RED",              !valid_sid(""));
    chk("traversal ../ RED",    !valid_sid("../../../etc/cron.d/pwn00000000000"));
    chk("hex-disi g RED",       !valid_sid("0123456789abcdef0123456789abcdeg"));
    chk("slash RED",            !valid_sid("0123456789abcdef0123456789abc/ef"));

    // ── blob enjeksiyon: kullanıcı verisi oturuma yazılıyor ──
    std::string blob;
    sess_blob_set(blob, "rol", "uye");                     // önce yetki
    // ÖLÇÜLEN SALDIRI: ?isim=bob\nrol=admin\nadmin=1
    sess_blob_set(blob, "isim", "bob\nrol=admin\nadmin=1");

    std::string out;
    chk("rol KORUNDU (=uye, admin degil)",
        sess_blob_get(blob, "rol", out) && out == "uye");
    chk("uydurma 'admin' alani YOK",
        !sess_blob_get(blob, "admin", out));
    chk("isim degeri gidis-donus AYNEN korunur",
        sess_blob_get(blob, "isim", out) && out == "bob\nrol=admin\nadmin=1");

    // anahtarda '=' → veri karıştırma denemesi: set("a=b","X") get("a") sızmamalı
    std::string b2;
    sess_blob_set(b2, "a=b", "X");
    chk("anahtar '=' kacirildi (get('a') b=X vermez)",
        !(sess_blob_get(b2, "a", out) && out == "b=X"));
    chk("gercek anahtar 'a=b' okunur",
        sess_blob_get(b2, "a=b", out) && out == "X");

    // overwrite: aynı key ikinci set → tek satır, yeni değer
    std::string b3;
    sess_blob_set(b3, "k", "v1");
    sess_blob_set(b3, "k", "v2");
    chk("overwrite: k=v2", sess_blob_get(b3, "k", out) && out == "v2");
    chk("overwrite: tek k satiri (v1 gitti)",
        b3.find("v1") == std::string::npos);

    // ters-bölü ve \r round-trip
    std::string b4;
    sess_blob_set(b4, "p", "a\\b\r\nc");
    chk("backslash + CRLF round-trip",
        sess_blob_get(b4, "p", out) && out == "a\\b\r\nc");
    // geriye-uyum: eski blob'da ters-bölü yok → unesc kimlik
    chk("eski-blob geriye-uyum (esc'siz duz metin)",
        [](){ std::string b="eski=duzmetin\n"; std::string o; return sess_blob_get(b,"eski",o) && o=="duzmetin"; }());

    const int EXPECTED = 17;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (SID traversal + blob \\n/= enjeksiyonu kilitli)\n", fails);
    return fails ? 1 : 0;
}

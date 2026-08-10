// SMTP saf parser tablo testi — smtp_extract_addr (dikişi aç, look/smtp_parse.h).
// Geçmiş bug'ları (bare adres, <>, iç içe <) kilitler + adversarial girdide ÇÖKME olmamalı.
//   Build (ASan): g++ -std=c++17 -Iinclude -fsanitize=address,undefined tests/smtp_parse_test.cpp -o /tmp/spt
#include "look/smtp_parse.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0;
static void chk(const char* in, bool want_ok, const char* want_addr) {
    std::string addr;
    bool ok = smtp_extract_addr(in, addr);
    bool pass = (ok == want_ok) && (!want_ok || addr == want_addr);
    printf("  %-34s -> ok=%d addr=%-14s %s\n", (std::string("\"")+in+"\"").c_str(),
           ok, ("\""+addr+"\"").c_str(), pass ? "OK" : "FAIL");
    if (!pass) fails++;
}

int main() {
    printf("SMTP extract_addr tablo (geçmiş bug'lar + adversarial):\n");
    // --- meşru ---
    chk("MAIL FROM:<a@b.com>",        true,  "a@b.com");
    chk("RCPT TO:<x@y.com>",          true,  "x@y.com");
    chk("MAIL FROM:<>",               true,  "");            // null sender (eskiden REDDEDİLİYORDU)
    chk("MAIL FROM: bare@x.com",      true,  "bare@x.com");  // bare (eskiden KIRIK'ti)
    chk("RCPT TO:x@y.com SIZE=100",   true,  "x@y.com");     // bare + ESMTP param → ilk token
    chk("MAIL FROM:<a@b.com> SIZE=9", true,  "a@b.com");     // angle + param
    // --- reddedilmeli (bozuk) ---
    chk("MAIL FROM",                  false, "");            // kolon yok
    chk("MAIL FROM:",                 false, "");            // adres yok
    chk("MAIL FROM:   ",              false, "");            // sadece boşluk
    chk("MAIL FROM:<a@b<c>",          false, "");            // iç içe '<'
    chk("MAIL FROM:<a b@c>",          false, "");            // adreste boşluk
    chk("MAIL FROM:<unclosed",        false, "");            // kapanmamış '<'
    chk("MAIL FROM:bare>evil",        false, "");            // bare + '>' → red

    // --- ADVERSARIAL: ÇÖKME OLMAMALI (ASan yakalar) — dönüş değeri önemsiz ---
    const char* adv[] = {
        "", ":", "::::", "<", ">", "<>", ":<", ":<<<<<<<<<<",
        "MAIL FROM:<\x01\x02\x03>", "MAIL FROM:<\xff\xfe>",
        "\r\n\r\n", "MAIL FROM:<a@b.com",
    };
    std::string big(100000, '<'); big = "MAIL FROM:" + big;   // 100K '<' — OOB/quadratic yem
    for (auto s : adv) { std::string a; (void)smtp_extract_addr(s, a); }
    { std::string a; (void)smtp_extract_addr(big, a); }
    printf("  adversarial+100K: ÇÖKME YOK (ASan temiz)\n");

    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (extract_addr sağlam + geçmiş bug'lar kilitli)\n", fails);
    return fails ? 1 : 0;
}

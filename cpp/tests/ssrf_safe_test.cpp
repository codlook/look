// SSRF private/özel-blok tablo testi — look/ssrf_safe.h (dikişi aç). Ağsız, DNS YOK.
// http_client.cpp'nin getaddrinfo döngüsü bu predikatlara delege eder; burada
// ham byte'lardan karar tablosu POZİTİF-KONTROLLÜ kilitlenir (metadata/NAT64/6to4/mapped).
//   Build: g++ -std=c++17 -Iinclude tests/ssrf_safe_test.cpp -o /tmp/ssrf && /tmp/ssrf
#include "look/ssrf_safe.h"
#include <cstdio>
#include <cstdint>
#include <string>
using namespace look;

static int fails = 0, ran = 0;

// IPv4: dört sekizli → host-order → beklenen engel
static void v4(const char* label, uint8_t a, uint8_t b, uint8_t c, uint8_t d, bool want) {
    ran++;
    uint32_t ip = ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)c<<8)|d;
    bool got = is_private_v4(ip);
    bool ok = (got == want);
    printf("  v4 %-22s %d.%d.%d.%d -> block=%d want=%d %s\n", label, a,b,c,d, got, want, ok?"OK":"FAIL");
    if (!ok) fails++;
}
// IPv6: 16 byte dizi → beklenen engel
static void v6(const char* label, const uint8_t b[16], bool want) {
    ran++;
    bool got = ssrf_blocked_v6(b);
    bool ok = (got == want);
    printf("  v6 %-22s -> block=%d want=%d %s\n", label, got, want, ok?"OK":"FAIL");
    if (!ok) fails++;
}
// v6 yardımcı: "::v4" tabanlı 16-byte kur (prefix ilk baytları, v4 son 4 bayta)
static void mk_mapped(uint8_t out[16], uint8_t a, uint8_t bb, uint8_t c, uint8_t d) {
    for (int i=0;i<16;i++) out[i]=0;
    out[10]=0xFF; out[11]=0xFF; out[12]=a; out[13]=bb; out[14]=c; out[15]=d;
}

int main() {
    printf("SSRF karar tablosu:\n");
    // IPv4 engelli aileler
    v4("loopback",     127,0,0,1,   true);
    v4("10/8",         10,0,0,1,     true);
    v4("172.16/12",    172,16,5,5,   true);
    v4("172.31 ust",   172,31,255,1, true);
    v4("192.168",      192,168,1,1,  true);
    v4("metadata",     169,254,169,254, true);   // cloud metadata — kritik
    v4("cgnat 100.64", 100,64,0,1,   true);
    v4("0/8",          0,0,0,0,      true);
    v4("multicast",    224,0,0,1,    true);
    v4("reserved 240", 240,0,0,1,    true);
    v4("broadcast",    255,255,255,255, true);
    // IPv4 İZİNLİ (public)
    v4("google dns",   8,8,8,8,      false);
    v4("public 1.1",   1,1,1,1,      false);
    v4("172.15 disi",  172,15,0,1,   false);     // 172.16/12 sınır-altı
    v4("172.32 disi",  172,32,0,1,   false);     // sınır-üstü
    v4("192.167 disi", 192,167,1,1,  false);

    // IPv6
    uint8_t buf[16];
    // ::1 loopback
    for(int i=0;i<16;i++) buf[i]=0; buf[15]=1; v6("::1 loopback", buf, true);
    // :: unspecified
    for(int i=0;i<16;i++) buf[i]=0; v6(":: unspecified", buf, true);
    // fc00::/7 unique-local
    for(int i=0;i<16;i++) buf[i]=0; buf[0]=0xFC; v6("fc00 unique-local", buf, true);
    for(int i=0;i<16;i++) buf[i]=0; buf[0]=0xFD; v6("fd00 unique-local", buf, true);
    // fe80::/10 link-local
    for(int i=0;i<16;i++) buf[i]=0; buf[0]=0xFE; buf[1]=0x80; v6("fe80 link-local", buf, true);
    // ::ffff:169.254.169.254 — mapped metadata (kritik gömülü-v4 bypass)
    mk_mapped(buf, 169,254,169,254); v6("mapped metadata", buf, true);
    // ::ffff:8.8.8.8 — mapped public → izinli
    mk_mapped(buf, 8,8,8,8); v6("mapped public", buf, false);
    // 64:ff9b::10.0.0.1 — NAT64 private
    for(int i=0;i<16;i++) buf[i]=0; buf[1]=0x64; buf[2]=0xFF; buf[3]=0x9B; buf[12]=10; buf[15]=1;
    v6("NAT64 private", buf, true);
    // 64:ff9b::8.8.8.8 — NAT64 public → izinli
    for(int i=0;i<16;i++) buf[i]=0; buf[1]=0x64; buf[2]=0xFF; buf[3]=0x9B; buf[12]=8; buf[13]=8; buf[14]=8; buf[15]=8;
    v6("NAT64 public", buf, false);
    // 2002:c0a8:: — 6to4 embedding 192.168.x → private
    for(int i=0;i<16;i++) buf[i]=0; buf[0]=0x20; buf[1]=0x02; buf[2]=192; buf[3]=168; buf[4]=0; buf[5]=1;
    v6("6to4 private", buf, true);
    // 2001:4860:: — public v6 (Google) → izinli
    for(int i=0;i<16;i++) buf[i]=0; buf[0]=0x20; buf[1]=0x01; buf[2]=0x48; buf[3]=0x60; buf[15]=0x88;
    v6("public v6", buf, false);

    const int EXPECTED = 27;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (metadata/NAT64/6to4/mapped bypass kilitli)\n", fails);
    return fails ? 1 : 0;
}

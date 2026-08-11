// MySQL multi-packet birleştirme tablo testi — look/mysql_wire_parse.h (dikişi aç).
// mysql_client.cpp read_packet bunu çağırır; reader = socket recv. Burada reader BELLEK-tamponu
// besler (canlı MySQL YOK) → 0xFFFFFF devam, cap, sıfır-uzunluk son-parça POZİTİF-KONTROLLÜ.
//   Build: g++ -std=c++17 -Iinclude tests/mysql_reassemble_test.cpp -o /tmp/mra && /tmp/mra
#include "look/mysql_wire_parse.h"
#include <cstdio>
#include <vector>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
static void ok(const char* label, bool cond) {
    ran++;
    printf("  %-42s %s\n", label, cond ? "OK" : "FAIL");
    if (!cond) fails++;
}

// Bir MySQL paketi (4-byte başlık + gövde) wire baytlarına ekle.
static void push_pkt(std::vector<uint8_t>& wire, uint32_t len, uint8_t seq, uint8_t fill) {
    wire.push_back((uint8_t)len); wire.push_back((uint8_t)(len>>8)); wire.push_back((uint8_t)(len>>16));
    wire.push_back(seq);
    for (uint32_t i = 0; i < len; ++i) wire.push_back(fill);
}
// wire tamponundan sıralı okuyan reader üretir.
struct Reader {
    const std::vector<uint8_t>& wire; size_t pos = 0;
    bool operator()(uint8_t* buf, size_t n) {
        if (pos + n > wire.size()) return false;
        for (size_t i = 0; i < n; ++i) buf[i] = wire[pos + i];
        pos += n; return true;
    }
};

int main() {
    printf("MySQL multi-packet birleştirme tablosu:\n");
    // ── tek paket (<0xFFFFFF) — döngü bir kez, tek-paket davranışı ──
    {
        std::vector<uint8_t> w; push_pkt(w, 5, 7, 0xAB);
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("tek paket: 5 bayt", p.size() == 5 && p[0] == 0xAB);
        ok("tek paket: seq korunur", seq == 7);
    }
    // ── sıfır-uzunluk paket → boş payload, akış senkron ──
    {
        std::vector<uint8_t> w; push_pkt(w, 0, 3, 0);
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("sıfır-uzunluk: boş payload", p.empty() && seq == 3);
    }
    // ── multi-packet: 0xFFFFFF devam + küçük son-parça → BİRLEŞTİR ──
    {
        std::vector<uint8_t> w;
        push_pkt(w, 0xFFFFFF, 0, 0x11);   // ilk tam parça (16MB-1)
        push_pkt(w, 10,       1, 0x22);   // son parça (<0xFFFFFF)
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("multi: toplam boyut = 0xFFFFFF+10", p.size() == (size_t)0xFFFFFF + 10);
        ok("multi: ilk parça baytı", p[0] == 0x11);
        ok("multi: son parça baytı", p[(size_t)0xFFFFFF + 5] == 0x22);
        ok("multi: son seq döner", seq == 1);
    }
    // ── multi-packet: TAM-0xFFFFFF ardından SIFIR-uzunluk son-parça (RFC gereği: payload tam
    //    16MB katıysa 0-uzunluk terminatör paketi gelir) → birleştirme burada biter ──
    {
        std::vector<uint8_t> w;
        push_pkt(w, 0xFFFFFF, 0, 0x33);
        push_pkt(w, 0,        1, 0);       // 0-uzunluk terminatör
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("multi+0-terminatör: boyut = 0xFFFFFF", p.size() == (size_t)0xFFFFFF);
    }
    // ── güvenlik cap: küçük max_recv aşılırsa fırlat (sonsuz-devam DoS savunması) ──
    {
        std::vector<uint8_t> w; push_pkt(w, 100, 0, 0x44);
        Reader r{w}; uint8_t seq = 0; bool threw = false;
        try { mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq, 50); }
        catch (const std::exception&) { threw = true; }
        ok("cap aşımı fırlatır (100 > max 50)", threw);
    }
    // ── eksik/kopuk akış: başlık okunamaz → fırlat ──
    {
        std::vector<uint8_t> w; w.push_back(0x05); // yarım başlık
        Reader r{w}; uint8_t seq = 0; bool threw = false;
        try { mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq); }
        catch (const std::exception&) { threw = true; }
        ok("kopuk başlık fırlatır", threw);
    }

    const int EXPECTED = 10;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (0xFFFFFF devam + cap + 0-terminatör kilitli)\n", fails);
    return fails ? 1 : 0;
}

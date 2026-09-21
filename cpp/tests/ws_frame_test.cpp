// WebSocket frame-decode adversarial tablo testi — look/websocket.h ws_try_decode_frame.
// Untrusted-NETWORK yüzeyi (websocket peer). Ağdan gelen SALDIRGAN-KONTROLLÜ frame baytları
// bu koddan geçer. Parser adversarial turu (2026-09-21): decode_frame'in birim-guard'ı YOKTU
// (yalnız close/load/tsan entegrasyon testleri vardı) — bu, ASan/UBSan-doğrulanmış güvenlik
// davranışını + RFC 6455 §5.2 konformans fix'lerini (RSV + ayrılmış-opcode) pinler.
//   Build: g++ -std=c++17 -Iinclude -fsanitize=address,undefined tests/ws_frame_test.cpp
//          src/websocket.cpp -o /tmp/wsf && /tmp/wsf
#include "look/websocket.h"
#include <cstdio>
#include <string>
#include <initializer_list>
using namespace look;

static int fails = 0, ran = 0;
static std::string B(std::initializer_list<int> xs){ std::string s; for(int x:xs) s.push_back((char)(uint8_t)x); return s; }
// beklenen: complete, protocol_error (opcode/payload yalnız complete=1 iken önemli)
static void chk(const char* tag, std::string buf, bool wcomplete, bool wperr) {
    ran++;
    WsFrame f = ws_try_decode_frame(buf);
    bool ok = (f.complete == wcomplete) && (f.protocol_error == wperr);
    printf("  %-30s -> complete=%d perr=%d %s\n", tag, f.complete, f.protocol_error, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("WebSocket ws_try_decode_frame adversarial tablosu:\n");
    // ── incomplete/boundary — daha çok bayt bekle (complete=0, perr=0) ──
    chk("empty",                 "",                              false, false);
    chk("1 byte",                B({0x81}),                       false, false);
    chk("masked len1, no body",  B({0x81,0x81}),                  false, false);
    chk("126 truncated",         B({0x81,0xFE,0x00}),             false, false);
    // ── geçerli maskeli frame'ler ──
    chk("masked len0",           B({0x81,0x80,0,0,0,0}),          true,  false);
    chk("masked len1",           B({0x81,0x81,0,0,0,0,'A'}),      true,  false);
    // ── GÜVENLİK: maskesiz client frame → protokol hatası (RFC §5.1) ──
    chk("unmasked len0",         B({0x81,0x00}),                  false, true);
    // ── GÜVENLİK: 64-bit uzunluk 16MB cap (dev resize DoS reddi, RFC §5.2) ──
    chk("127 huge 2^63",         B({0x82,0xFF,0x80,0,0,0,0,0,0,0,0,0,0,0}), false, false);
    chk("127 just over 16MB",    B({0x82,0xFF,0,0,0,0,0x01,0x00,0x00,0x01,0,0,0,0}), false, false);
    // ── GÜVENLİK: kontrol frame ≤125 + parçalanamaz (RFC §5.5) ──
    chk("close plen=126",        B({0x88,0xFE,0x00,0x7E}),        false, true);
    chk("ping FIN=0 fragmented", B({0x09,0x80,0,0,0,0}),          false, true);
    // ── KONFORMANS FIX (2026-09-21): ayrılmış opcode → protokol hatası (eskiden sessiz kabul) ──
    chk("reserved opcode 0x3",   B({0x83,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0x7",   B({0x87,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0xB",   B({0x8B,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0xF",   B({0x8F,0x80,0,0,0,0}),          false, true);
    // ── KONFORMANS FIX: sıfır-dışı RSV → protokol hatası (uzantı yok, eskiden sessiz kabul) ──
    chk("RSV1 set (0xC1)",       B({0xC1,0x80,0,0,0,0}),          false, true);
    chk("RSV2 set (0xA1)",       B({0xA1,0x80,0,0,0,0}),          false, true);
    chk("RSV3 set (0x91)",       B({0x91,0x80,0,0,0,0}),          false, true);
    // ── negatif kontrol: geçerli pong (0xA) ayrılmış SAYILMAMALI ──
    chk("valid pong 0xA",        B({0x8A,0x80,0,0,0,0}),          true,  false);

    const int EXPECTED = 19;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (ws decode güvenlik + RFC §5.2 konformans kilitli)\n", fails);
    return fails ? 1 : 0;
}

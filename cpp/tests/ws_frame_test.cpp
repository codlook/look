// WebSocket frame-decode adversarial table test — look/websocket.h ws_try_decode_frame.
// Untrusted-NETWORK surface (websocket peer). ATTACKER-CONTROLLED frame bytes from the network
// pass through this code. Parser adversarial round (2026-09-21): decode_frame had NO unit guard
// (there were only close/load/tsan integration tests) — this pins the ASan/UBSan-verified security
// behavior + the RFC 6455 §5.2 conformance fixes (RSV + reserved opcode).
//   Build: g++ -std=c++17 -Iinclude -fsanitize=address,undefined tests/ws_frame_test.cpp
//          src/websocket.cpp -o /tmp/wsf && /tmp/wsf
#include "look/websocket.h"
#include <cstdio>
#include <string>
#include <initializer_list>
using namespace look;

static int fails = 0, ran = 0;
static std::string B(std::initializer_list<int> xs){ std::string s; for(int x:xs) s.push_back((char)(uint8_t)x); return s; }
// expected: complete, protocol_error (opcode/payload matter only when complete=1)
static void chk(const char* tag, std::string buf, bool wcomplete, bool wperr) {
    ran++;
    WsFrame f = ws_try_decode_frame(buf);
    bool ok = (f.complete == wcomplete) && (f.protocol_error == wperr);
    printf("  %-30s -> complete=%d perr=%d %s\n", tag, f.complete, f.protocol_error, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("WebSocket ws_try_decode_frame adversarial tablosu:\n");
    // ── incomplete/boundary — wait for more bytes (complete=0, perr=0) ──
    chk("empty",                 "",                              false, false);
    chk("1 byte",                B({0x81}),                       false, false);
    chk("masked len1, no body",  B({0x81,0x81}),                  false, false);
    chk("126 truncated",         B({0x81,0xFE,0x00}),             false, false);
    // ── valid masked frames ──
    chk("masked len0",           B({0x81,0x80,0,0,0,0}),          true,  false);
    chk("masked len1",           B({0x81,0x81,0,0,0,0,'A'}),      true,  false);
    // ── SECURITY: unmasked client frame → protocol error (RFC §5.1) ──
    chk("unmasked len0",         B({0x81,0x00}),                  false, true);
    // ── SECURITY: 64-bit length 16MB cap (huge-resize DoS rejection, RFC §5.2) ──
    // A frame declaring >16 MB is a PROTOCOL ERROR (the connection closes). It used to be "wait for more bytes":
    // every byte the client streamed was buffered forever (memory DoS, a9c74a0).
    chk("127 huge 2^63",         B({0x82,0xFF,0x80,0,0,0,0,0,0,0,0,0,0,0}), false, true);
    chk("127 just over 16MB",    B({0x82,0xFF,0,0,0,0,0x01,0x00,0x00,0x01,0,0,0,0}), false, true);
    // ── SECURITY: control frame ≤125 + not fragmentable (RFC §5.5) ──
    chk("close plen=126",        B({0x88,0xFE,0x00,0x7E}),        false, true);
    chk("ping FIN=0 fragmented", B({0x09,0x80,0,0,0,0}),          false, true);
    // ── CONFORMANCE FIX (2026-09-21): reserved opcode → protocol error (used to be silently accepted) ──
    chk("reserved opcode 0x3",   B({0x83,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0x7",   B({0x87,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0xB",   B({0x8B,0x80,0,0,0,0}),          false, true);
    chk("reserved opcode 0xF",   B({0x8F,0x80,0,0,0,0}),          false, true);
    // ── CONFORMANCE FIX: non-zero RSV → protocol error (no extensions, used to be silently accepted) ──
    chk("RSV1 set (0xC1)",       B({0xC1,0x80,0,0,0,0}),          false, true);
    chk("RSV2 set (0xA1)",       B({0xA1,0x80,0,0,0,0}),          false, true);
    chk("RSV3 set (0x91)",       B({0x91,0x80,0,0,0,0}),          false, true);
    // ── negative control: a valid pong (0xA) must NOT count as reserved ──
    chk("valid pong 0xA",        B({0x8A,0x80,0,0,0,0}),          true,  false);

    const int EXPECTED = 19;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (ws decode güvenlik + RFC §5.2 konformans kilitli)\n", fails);
    return fails ? 1 : 0;
}

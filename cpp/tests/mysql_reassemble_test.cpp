// MySQL multi-packet reassembly table test — look/mysql_wire_parse.h (open the seam).
// mysql_client.cpp read_packet calls this; reader = socket recv. Here the reader is fed a MEMORY buffer
// (NO live MySQL) → 0xFFFFFF continuation, cap, zero-length last-part WITH POSITIVE CONTROLS.
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

// Append a MySQL packet (4-byte header + body) to the wire bytes.
static void push_pkt(std::vector<uint8_t>& wire, uint32_t len, uint8_t seq, uint8_t fill) {
    wire.push_back((uint8_t)len); wire.push_back((uint8_t)(len>>8)); wire.push_back((uint8_t)(len>>16));
    wire.push_back(seq);
    for (uint32_t i = 0; i < len; ++i) wire.push_back(fill);
}
// builds a reader that reads sequentially from the wire buffer.
struct Reader {
    const std::vector<uint8_t>& wire; size_t pos = 0;
    bool operator()(uint8_t* buf, size_t n) {
        if (pos + n > wire.size()) return false;
        for (size_t i = 0; i < n; ++i) buf[i] = wire[pos + i];
        pos += n; return true;
    }
};

int main() {
    printf("MySQL multi-packet reassembly table:\n");
    // ── single packet (<0xFFFFFF) — loop once, single-packet behavior ──
    {
        std::vector<uint8_t> w; push_pkt(w, 5, 7, 0xAB);
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("single packet: 5 bytes", p.size() == 5 && p[0] == 0xAB);
        ok("tek paket: seq korunur", seq == 7);
    }
    // ── zero-length packet → empty payload, stream in sync ──
    {
        std::vector<uint8_t> w; push_pkt(w, 0, 3, 0);
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("zero-length: empty payload", p.empty() && seq == 3);
    }
    // ── multi-packet: 0xFFFFFF continuation + small last part → REASSEMBLE ──
    {
        std::vector<uint8_t> w;
        push_pkt(w, 0xFFFFFF, 0, 0x11);   // first full part (16MB-1)
        push_pkt(w, 10,       1, 0x22);   // last part (<0xFFFFFF)
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("multi: toplam boyut = 0xFFFFFF+10", p.size() == (size_t)0xFFFFFF + 10);
        ok("multi: first-chunk byte", p[0] == 0x11);
        ok("multi: last-chunk byte", p[(size_t)0xFFFFFF + 5] == 0x22);
        ok("multi: last seq is returned", seq == 1);
    }
    // ── multi-packet: EXACT 0xFFFFFF followed by a ZERO-length last part (per the RFC: when the payload is exactly
    //    a multiple of 16MB a 0-length terminator packet follows) → reassembly ends here ──
    {
        std::vector<uint8_t> w;
        push_pkt(w, 0xFFFFFF, 0, 0x33);
        push_pkt(w, 0,        1, 0);       // 0-length terminator
        Reader r{w}; uint8_t seq = 0;
        auto p = mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq);
        ok("multi+0-terminator: size = 0xFFFFFF", p.size() == (size_t)0xFFFFFF);
    }
    // ── security cap: throw if a small max_recv is exceeded (defense against endless-continuation DoS) ──
    {
        std::vector<uint8_t> w; push_pkt(w, 100, 0, 0x44);
        Reader r{w}; uint8_t seq = 0; bool threw = false;
        try { mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq, 50); }
        catch (const std::exception&) { threw = true; }
        ok("exceeding the cap throws (100 > max 50)", threw);
    }
    // ── missing/broken stream: header cannot be read → throw ──
    {
        std::vector<uint8_t> w; w.push_back(0x05); // half header
        Reader r{w}; uint8_t seq = 0; bool threw = false;
        try { mysql_reassemble([&r](uint8_t* b, size_t n){ return r(b, n); }, seq); }
        catch (const std::exception&) { threw = true; }
        ok("truncated header throws", threw);
    }

    const int EXPECTED = 10;
    if (ran != EXPECTED) { printf("\nFAIL: %d cases expected, %d ran\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nALL CASES PASSED (0xFFFFFF continuation + cap + 0-terminator locked in)\n", fails);
    return fails ? 1 : 0;
}

// FastCGI wire-protocol pure parsers — SEPARATE from the socket (fuzz + table-test target).
// "Unstitch": FCGI record header decoding + FCGI_PARAMS name/value-length reading logic
// live here. ATTACKER-CONTROLLED bytes coming from the network (nginx/Apache → FCGI, or exposed to the network directly with LOOK_FCGI_BIND)
// pass through this code. A single definition (fcgi_main.cpp includes it) →
// no drift. contentLength is bounded by uint16, padding by uint8, and name/value-length by being clamped to
// the buffer end (end) → no OOB.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <map>

namespace look {

// ── Record header (8 bytes, fixed layout) ─────────────────────────────────────
// FastCGI 1.0 record header:
//   byte0: version, byte1: type, byte2..3: requestId (BE),
//   byte4..5: contentLength (BE), byte6: paddingLength, byte7: reserved.
// contentLength uint16 (≤65535), paddingLength uint8 (≤255) → resize is safe.
struct FcgiRecordHeader {
    uint8_t  version      = 0;
    uint8_t  type         = 0;
    uint16_t req_id       = 0;
    uint16_t content_len  = 0;
    uint8_t  padding_len  = 0;
};

// Decode the 8-byte header. The caller must guarantee at least 8 bytes (raw_read(hdr,8)).
inline FcgiRecordHeader fcgi_decode_header(const uint8_t hdr[8]) {
    FcgiRecordHeader h;
    h.version     = hdr[0];
    h.type        = hdr[1];
    h.req_id      = (uint16_t)((hdr[2] << 8) | hdr[3]);
    h.content_len = (uint16_t)((hdr[4] << 8) | hdr[5]);
    h.padding_len = hdr[6];
    return h;
}

// KEEP_CONN flag from the BEGIN_REQUEST body. content[2] & FCGI_KEEP_CONN(0x01).
// The caller must check content.size() >= 8 (the spec's BEGIN_REQUEST body is exactly 8 bytes).
inline bool fcgi_begin_keep_conn(const uint8_t* content, size_t len) {
    if (len < 3) return false;
    return (content[2] & 0x01) != 0;
}

// ── FCGI_PARAMS name/value-length reading (1 or 4 bytes, high bit flagged) ─────
// OVERFLOW-SAFE: if the high bit is set 4 bytes are needed → 0 if p+4>end. The p<end precondition
// comes from the caller (parse_params `while (p<end)`).
inline uint32_t fcgi_read_len(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    if ((*p >> 7) == 0) return *p++;
    if (p + 4 > end) return 0;
    uint32_t v = ((uint32_t)(p[0] & 0x7F) << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    p += 4;
    return v;
}

// Parse the FCGI_PARAMS content as (name-len, value-len, name, value)*.
// OVERFLOW-SAFE: nlen/vlen can be uint32 (≤2^31) but the `p + nlen + vlen > end` check
// (NO wrap in the 64-bit address space; content ≤65535) TRUNCATES (break) a record that exceeds the bound →
// OOB read is impossible. Same name repeated → the last value wins (std::map insert-or-assign).
//
// INFINITE-LOOP PROTECTION: if the length field has the high bit BUT the 4 bytes are not complete
// (truncated record), fcgi_read_len returns 0 and does NOT ADVANCE p; without the protection
// `while (p<end)` could never advance p and would loop forever → a single malformed PARAMS
// record would lock the worker thread forever (DoS). `p == q` (no progress) → break.
inline void fcgi_parse_params(const uint8_t* p, size_t len,
                              std::map<std::string, std::string>& out) {
    const uint8_t* end = p + len;
    while (p < end) {
        const uint8_t* q = p;
        uint32_t nlen = fcgi_read_len(p, end);
        uint32_t vlen = fcgi_read_len(p, end);
        if (p == q) break;                    // truncated length field → no progress, STOP
        if (p + nlen + vlen > end) break;
        std::string name (p, p + nlen); p += nlen;
        std::string value(p, p + vlen); p += vlen;
        out[name] = value;
    }
}

} // namespace look

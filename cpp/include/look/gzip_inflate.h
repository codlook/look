#pragma once
// gzip response body decoding — a pure function (no socket): used by http_client, the fuzz target calls it directly
// (tests/fuzz/fuzz_gzip_inflate.cpp). The input comes from whatever server http::get connects to:
// it is the attacker's bytes (Node2Shell / CVE-2026-111497 class: the client path to an embedded decoder).
//
// The gzip header is parsed by hand, the payload is raw-inflated with miniz (window_bits < 0). The output is bounded by `cap`
// (gzip bomb). A corrupt stream is not confused with "more data needed": every round that returns without
// progress is an error (it used to keep looping on MZ_BUF_ERROR while input remained).
#include "miniz/miniz.h"
#include <cstring>
#include <string>

namespace look {

inline bool gzip_inflate(const std::string& in, size_t cap, std::string& out) {
    out.clear();
    if (in.size() < 18) return false;                       // 10-byte hdr + 8-byte trailer min
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08) return false;  // magic + DEFLATE method
    unsigned char flg = p[3];
    if (flg & 0xE0) return false;                            // RFC 1952: reserved flags must be zero
    size_t off = 10;
    if (flg & 0x04) {                                       // FEXTRA
        if (off + 2 > in.size()) return false;
        size_t xlen = (size_t)p[off] | ((size_t)p[off + 1] << 8);
        off += 2 + xlen;
        if (off > in.size()) return false;
    }
    if (flg & 0x08) { while (off < in.size() && p[off] != 0) ++off; ++off; }  // FNAME
    if (flg & 0x10) { while (off < in.size() && p[off] != 0) ++off; ++off; }  // FCOMMENT
    if (flg & 0x02) off += 2;                               // FHCRC
    if (off >= in.size()) return false;
    if (in.size() - off > 0xFFFFFFFFu) return false;         // avail_in 32 bit

    mz_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (mz_inflateInit2(&strm, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;  // raw deflate
    strm.next_in  = p + off;
    strm.avail_in = (unsigned int)(in.size() - off);        // trailer left over; inflate stops at stream end

    unsigned char obuf[16384];
    int ret;
    for (;;) {
        strm.next_out  = obuf;
        strm.avail_out = sizeof(obuf);
        const unsigned int in_before = strm.avail_in;
        ret = mz_inflate(&strm, MZ_NO_FLUSH);
        const size_t produced = sizeof(obuf) - strm.avail_out;
        out.append(reinterpret_cast<const char*>(obuf), produced);
        if (out.size() > cap) { mz_inflateEnd(&strm); return false; }   // bomb guard
        if (ret == MZ_STREAM_END) break;
        if (ret != MZ_OK && ret != MZ_BUF_ERROR) { mz_inflateEnd(&strm); return false; }
        // If there is no progress (no input consumed, no output produced) the stream is corrupt or incomplete: stop.
        if (produced == 0 && strm.avail_in == in_before) { mz_inflateEnd(&strm); return false; }
        if (strm.avail_in == 0 && produced < sizeof(obuf)) { mz_inflateEnd(&strm); return false; }  // input ended, no end of stream
    }
    mz_inflateEnd(&strm);
    return true;
}

} // namespace look

#pragma once
// gzip yanıt gövdesi çözme — saf işlev (soket yok): http_client kullanır, fuzz hedefi doğrudan çağırır
// (tests/fuzz/fuzz_gzip_inflate.cpp). Girdi, http::get'in bağlandığı herhangi bir sunucudan gelir:
// saldırganın baytlarıdır (Node2Shell / CVE-2026-111497 sınıfı: gömülü çözücüye istemci yolu).
//
// gzip başlığı elle ayrıştırılır, yük miniz ile ham inflate edilir (window_bits < 0). Çıktı `cap`
// ile sınırlı (gzip bombası). Bozuk akış "daha fazla veri gerek" ile karıştırılmaz: ilerleme
// olmadan dönen her tur hatadır (eskiden MZ_BUF_ERROR'da girdi kaldıkça döngü sürüyordu).
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
    if (flg & 0xE0) return false;                            // RFC 1952: ayrılmış bayraklar sıfır olmalı
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
        // İlerleme yoksa (girdi tüketilmedi, çıktı üretilmedi) akış bozuk ya da eksik: dur.
        if (produced == 0 && strm.avail_in == in_before) { mz_inflateEnd(&strm); return false; }
        if (strm.avail_in == 0 && produced < sizeof(obuf)) { mz_inflateEnd(&strm); return false; }  // girdi bitti, son yok
    }
    mz_inflateEnd(&strm);
    return true;
}

} // namespace look

// FastCGI wire-protocol saf ayrıştırıcılar — socket'ten AYRI (fuzz + tablo-test hedefi).
// "Dikişi aç": FCGI record header çözme + FCGI_PARAMS name/value-length okuma mantığı
// burada. Ağdan (nginx/Apache → FCGI, ya da doğrudan LOOK_FCGI_BIND ile ağa açık) gelen
// SALDIRGAN-KONTROLLÜ baytlar bu koddan geçer. Tek tanım (fcgi_main.cpp include eder) →
// drift yok. contentLength uint16 ile, padding uint8 ile, name/value-length ise buffer
// sonuna (end) clamp'lenerek sınırlanır → OOB yok.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <map>

namespace look {

// ── Record header (8 bayt, sabit düzen) ───────────────────────────────────────
// FastCGI 1.0 record header:
//   byte0: version, byte1: type, byte2..3: requestId (BE),
//   byte4..5: contentLength (BE), byte6: paddingLength, byte7: reserved.
// contentLength uint16 (≤65535), paddingLength uint8 (≤255) → resize güvenli.
struct FcgiRecordHeader {
    uint8_t  version      = 0;
    uint8_t  type         = 0;
    uint16_t req_id       = 0;
    uint16_t content_len  = 0;
    uint8_t  padding_len  = 0;
};

// 8-baytlık header'ı çöz. Çağıran en az 8 bayt olduğunu garanti etmeli (raw_read(hdr,8)).
inline FcgiRecordHeader fcgi_decode_header(const uint8_t hdr[8]) {
    FcgiRecordHeader h;
    h.version     = hdr[0];
    h.type        = hdr[1];
    h.req_id      = (uint16_t)((hdr[2] << 8) | hdr[3]);
    h.content_len = (uint16_t)((hdr[4] << 8) | hdr[5]);
    h.padding_len = hdr[6];
    return h;
}

// BEGIN_REQUEST gövdesinden KEEP_CONN bayrağı. content[2] & FCGI_KEEP_CONN(0x01).
// Çağıran content.size() >= 8 kontrolünü yapmalı (spec BEGIN_REQUEST gövdesi tam 8 bayt).
inline bool fcgi_begin_keep_conn(const uint8_t* content, size_t len) {
    if (len < 3) return false;
    return (content[2] & 0x01) != 0;
}

// ── FCGI_PARAMS name/value-length okuma (1 veya 4 bayt, yüksek-bit işaretli) ───
// TAŞMA-GÜVENLİ: yüksek bit set ise 4 bayt gerekir → p+4>end ise 0. p<end ön-koşulu
// çağırandan gelir (parse_params `while (p<end)`).
inline uint32_t fcgi_read_len(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    if ((*p >> 7) == 0) return *p++;
    if (p + 4 > end) return 0;
    uint32_t v = ((uint32_t)(p[0] & 0x7F) << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    p += 4;
    return v;
}

// FCGI_PARAMS içeriğini (name-len, value-len, name, value)* olarak ayrıştır.
// TAŞMA-GÜVENLİ: nlen/vlen uint32 (≤2^31) olabilir ama `p + nlen + vlen > end` kontrolü
// (64-bit adres alanında wrap YOK; içerik ≤65535) sınırı aşan kaydı KESER (break) →
// OOB read imkânsız. Aynı isim tekrarı → son değer kazanır (std::map insert-or-assign).
//
// SONSUZ-DÖNGÜ KORUMASI: uzunluk alanı yüksek-bitli AMA 4 bayt tamamlanmamışsa
// (kesik record), fcgi_read_len 0 döndürür ve p'yi İLERLETMEZ; koruma olmadan
// `while (p<end)` p'yi asla ilerletemeyip sonsuza döner → tek kötü-biçimli PARAMS
// record'u worker thread'i sonsuza kilitler (DoS). `p == q` (ilerleme yok) → break.
inline void fcgi_parse_params(const uint8_t* p, size_t len,
                              std::map<std::string, std::string>& out) {
    const uint8_t* end = p + len;
    while (p < end) {
        const uint8_t* q = p;
        uint32_t nlen = fcgi_read_len(p, end);
        uint32_t vlen = fcgi_read_len(p, end);
        if (p == q) break;                    // kesik uzunluk alanı → ilerleme yok, DUR
        if (p + nlen + vlen > end) break;
        std::string name (p, p + nlen); p += nlen;
        std::string value(p, p + vlen); p += vlen;
        out[name] = value;
    }
}

} // namespace look

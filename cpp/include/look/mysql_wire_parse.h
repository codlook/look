// MySQL wire-protocol saf ayrıştırıcılar — socket'ten AYRI (fuzz + test hedefi).
// "Dikişi aç": length-encoded int/str okuma mantığı burada; tarihsel OOB ("MySQL hata
// paketinde OOB okuma") tam bu sınıftaydı. Tek tanım (mysql_client.cpp include eder) →
// drift yok. Saldırgan-kontrollü baytlar: sunucu paketi gövdesi (S4 verify kapalı → MITM).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace look {

// length-encoded integer (MySQL protokolü). Her multi-byte durum p+N<=end ile sınırlı.
inline uint64_t mysql_read_lenenc(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    uint8_t first = *p++;
    if (first < 0xFB) return first;
    if (first == 0xFC && p + 2 <= end) { uint64_t v = p[0] | ((uint64_t)p[1] << 8); p += 2; return v; }
    if (first == 0xFD && p + 3 <= end) { uint64_t v = p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16); p += 3; return v; }
    if (first == 0xFE && p + 8 <= end) { uint64_t v; memcpy(&v, p, 8); p += 8; return v; }
    return 0;
}

// length-encoded string. TAŞMA-GÜVENLİ: len sunucudan gelir ve 0xFE'de ~2^64 olabilir →
// `p+len` işaretçi taşması (UB) yapıp wrap'lar → kontrol atlanır → ~2^64 overread. p<=end
// invariantı gereği `end-p` güvenli; len'i mevcut bayta clamp'le.
inline std::string mysql_read_lenenc_str(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return "";
    if (*p == 0xFB) { p++; return ""; }
    uint64_t len = mysql_read_lenenc(p, end);
    uint64_t avail = (uint64_t)(end - p);
    if (len > avail) len = avail;
    std::string s(p, p + (size_t)len); p += len;
    return s;
}

// Sabit-genişlik okuyucular — end YOK, çağıran p+N<=end guard'lamalı (fixed-layout yollar).
inline uint16_t mysql_read_u16(const uint8_t*& p) { uint16_t v = p[0] | (p[1] << 8); p += 2; return v; }
inline uint32_t mysql_read_u32(const uint8_t*& p) { uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
inline uint64_t mysql_read_u64(const uint8_t*& p) { uint64_t v; memcpy(&v, p, 8); p += 8; return v; }

} // namespace look

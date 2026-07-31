// PostgreSQL wire-protocol saf ayrıştırıcılar — socket'ten AYRI (fuzz + test hedefi).
// "Dikişi aç": bayt-yorumlama mantığı burada, I/O postgres_client.cpp'de. Tek tanım
// (postgres_client.cpp bunu include eder) → drift yok.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace look {

// ErrorResponse gövdesini ayrıştır: (field-type byte, NUL-terminated değer)* dizisi,
// çift-NUL ile biter. 'M' (human-readable message) alanını döndür, yoksa "unknown error".
// Saldırgan-kontrollü baytlar (sunucu ERR paketi) → her erişim p<end ile sınırlı.
inline std::string pg_parse_error(const std::vector<uint8_t>& body) {
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();
    while (p < end && *p != '\0') {
        char ft = (char)*p++;
        std::string fv;
        while (p < end && *p != '\0') fv += (char)*p++;
        if (p < end) p++;
        if (ft == 'M') return fv;
    }
    return "unknown error";
}

} // namespace look

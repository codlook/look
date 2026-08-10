#pragma once
// SMTP saf ayrıştırıcılar — "dikişi aç" (pg_parse.h / dkim_tag.h deseni): ağdan-beslenen,
// PRE-AUTH parse mantığını I/O'dan ayır → tek tanım (drift yok), fuzz + tablo testi mümkün.
// smtp_server.cpp bu header'ı include eder; ayrı .cpp/.static tanım YOK.
//
// extract_addr: `MAIL FROM:<addr>` / `RCPT TO:<addr>` parametresinden adresi çıkar (RFC 5321 §4.1.2).
//   Geçmiş bug'lar (kodda belgeli): `MAIL FROM:` bare adres KIRIK'ti, `<>` null-sender reddediliyordu,
//   iç içe `<`/boşluk çöp adres kabul ediliyordu. Şimdi bounds-checked + katı. Ağa açık → fuzz hedefi.
#include <string>
#include <cctype>

namespace look {

// "MAIL FROM:<a@b.com>" → addr="a@b.com", true.  "<>" → addr="", true (null sender).
// Bozuk (kapanmamış '<', iç içe '<', adreste boşluk, parametre yok) → false.
inline bool smtp_extract_addr(const std::string& line, std::string& addr) {
    addr.clear();
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;          // "MAIL FROM" — parametre yok
    std::string rest = line.substr(colon + 1);
    size_t b = rest.find_first_not_of(" \t");
    if (b == std::string::npos) return false;              // "MAIL FROM:" — adres yok
    rest = rest.substr(b);

    if (rest[0] == '<') {
        size_t gt = rest.find('>');
        if (gt == std::string::npos) return false;         // kapanmamış açı parantezi
        addr = rest.substr(1, gt - 1);
        // İç içe '<' veya boşluk = bozuk adres (RFC'de yol/adres içinde olamaz)
        if (addr.find('<') != std::string::npos ||
            addr.find(' ') != std::string::npos) { addr.clear(); return false; }
        return true;                                       // addr boş olabilir → null sender
    }
    // Açı parantezsiz (bare) adres — ESMTP parametrelerinden (SIZE=, BODY=) önceki ilk token
    size_t sp = rest.find_first_of(" \t");
    addr = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    if (addr.empty() ||
        addr.find('<') != std::string::npos ||
        addr.find('>') != std::string::npos) { addr.clear(); return false; }
    return true;
}

// Komut satırından fiili (verb) çıkar: ilk boşluk/CR/LF'e kadar, upper-case.
inline std::string smtp_verb(const std::string& line) {
    std::string v;
    for (char c : line) {
        if (c == ' ' || c == '\r' || c == '\n') break;
        v += (char)std::toupper((unsigned char)c);
    }
    return v;
}

} // namespace look

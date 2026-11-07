// JSON string-unescape ayrıştırıcısı — Value'dan bağımsız (fuzz + test hedefi).
// En yüksek maruziyetli yüzey sınıfı: request::json() / json::decode her API gövdesini,
// kimliksiz uzak istemciden, işler. \u surrogate-pair + escape mantığı en kırılgan kısım.
// Tek tanım (web_stdlib.cpp include eder) → drift yok. "Dikişi aç".
#pragma once
#include <cstdint>
#include <string>

namespace look {

inline void json_append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}
inline int json_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// \uXXXX'ten 4 hane hex oku (i, 'u'nun sonrasına konumlanır); geçersizse -1.
inline int json_read_u4(const std::string& s, size_t& i) {
    if (i + 4 > s.size()) return -1;
    int cp = 0;
    for (int k = 0; k < 4; k++) {
        int h = json_hexval(s[i + k]);
        if (h < 0) return -1;
        cp = (cp << 4) | h;
    }
    i += 4;
    return cp;
}
// RFC 8259 / Go encoding/json kuralları. Dönüş false = geçersiz string (çağıran tüm belgeyi
// reddeder). ESKİ GEVŞEKLİK: bilinmeyen kaçış ("\x" → "x"), bozuk \u ("\u12" → "\\u12"), ham
// kontrol karakteri ve kapanmamış string kabul ediliyordu — bir ağ geçidi aynı gövdeyi geçersiz
// sayıp LOOK geçerli sayınca ayrışma (HTTP smuggling'in JSON karşılığı). Eşi olmayan surrogate
// Go gibi U+FFFD olur (eskiden geçersiz UTF-8 — CESU — üretiyordu). i: açılış '"' üzerinde.
inline bool json_decode_str(const std::string& s, size_t& i, std::string& result) {
    i++; // skip "
    result.clear();
    while (i < s.size() && s[i] != '"') {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) return false;                       // ham kontrol karakteri
        if (c != '\\') { result += (char)c; i++; continue; }
        i++;
        if (i >= s.size()) return false;
        switch (s[i]) {
            case '"':  result += '"';  break;
            case '\\': result += '\\'; break;
            case '/':  result += '/';  break;
            case 'n':  result += '\n'; break;
            case 't':  result += '\t'; break;
            case 'r':  result += '\r'; break;
            case 'b':  result += '\b'; break;
            case 'f':  result += '\f'; break;
            case 'u': {
                i++;  // 'u'yu geç
                int cp = json_read_u4(s, i);
                if (cp < 0) return false;                 // \u ardından 4 hex hane şart
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    size_t save = i;
                    int lo = -1;
                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; lo = json_read_u4(s, i); }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { i = save; cp = 0xFFFD; }       // eşsiz yüksek surrogate
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;                          // eşsiz düşük surrogate
                }
                json_append_utf8(result, (uint32_t)cp);
                continue;  // i zaten ilerledi
            }
            default: return false;                        // bilinmeyen kaçış
        }
        i++;
    }
    if (i >= s.size()) return false;                      // kapanmamış string
    i++; // skip closing "
    return true;
}

} // namespace look

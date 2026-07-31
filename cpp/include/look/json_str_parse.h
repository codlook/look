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
inline std::string json_decode_str(const std::string& s, size_t& i) {
    i++; // skip "
    std::string result;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') {
            i++;
            if (i >= s.size()) break;   // yalniz backslash sonda — s[i]=s[size()] tanimli
                                        // ('\0') ama kirilgan; acikca sinirla.
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
                    // \uXXXX unicode escape — surrogate çifti (emoji vb.) birleştir
                    i++;  // 'u'yu geç
                    int cp = json_read_u4(s, i);
                    if (cp < 0) { result += "\\u"; break; }
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        i + 1 < s.size() && s[i] == '\\' && s[i+1] == 'u') {
                        size_t save = i; i += 2;
                        int lo = json_read_u4(s, i);
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { i = save; }  // eşleşmeyen — yalnız yüksek surrogate
                    }
                    json_append_utf8(result, (uint32_t)cp);
                    continue;  // i zaten ilerledi; sondaki i++ atla
                }
                default:   result += s[i]; break;
            }
        } else {
            result += s[i];
        }
        i++;
    }
    i++; // skip closing "
    return result;
}

} // namespace look

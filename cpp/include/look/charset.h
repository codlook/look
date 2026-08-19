// charset.h — convert common single-byte encodings to UTF-8.
// Header-only, one code path (no iconv / Win32), no external dependency.
// Handles iso-8859-1 / iso-8859-9 / windows-1252 / windows-1254 — enough for the
// Turkish web (iso-8859-9 / windows-1254). Multi-byte charsets are left untouched.
#pragma once
#include <string>
#include <cctype>

namespace look {

inline void utf8_append(std::string& out, unsigned int cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// windows-1252 high block 0x80–0x9F (0 = undefined → keep the byte value)
inline const unsigned int* cp1252_hi() {
    static const unsigned int T[32] = {
        0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
        0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178
    };
    return T;
}

// Convert `in` (bytes in charset `cs`) to UTF-8 in `out`. Returns false when `cs`
// is not one we handle — the caller then leaves the input untouched.
inline bool charset_to_utf8(const std::string& in, const std::string& cs, std::string& out) {
    std::string c;
    for (char ch : cs) if (ch != '-' && ch != '_' && ch != ' ') c += (char)std::tolower((unsigned char)ch);
    bool s9  = (c == "iso88599"   || c == "latin5");
    bool s1  = (c == "iso88591"   || c == "latin1");
    bool w54 = (c == "windows1254"|| c == "cp1254");
    bool w52 = (c == "windows1252"|| c == "cp1252");
    if (!s9 && !s1 && !w54 && !w52) return false;

    const unsigned int* hi = cp1252_hi();
    out.clear(); out.reserve(in.size() + in.size() / 8);
    for (unsigned char b : in) {
        unsigned int cp = b;
        if (b >= 0x80) {
            if ((w52 || w54) && b <= 0x9F) { unsigned int m = hi[b - 0x80]; if (m) cp = m; }
            if (s9 || w54) {
                switch (b) {
                    case 0xD0: cp = 0x011E; break;  // Ğ
                    case 0xDD: cp = 0x0130; break;  // İ
                    case 0xDE: cp = 0x015E; break;  // Ş
                    case 0xF0: cp = 0x011F; break;  // ğ
                    case 0xFD: cp = 0x0131; break;  // ı
                    case 0xFE: cp = 0x015F; break;  // ş
                }
            }
        }
        utf8_append(out, cp);
    }
    return true;
}

} // namespace look

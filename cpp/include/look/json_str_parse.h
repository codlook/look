// JSON string-unescape parser — independent of Value (fuzz + test target).
// The highest-exposure surface class: request::json() / json::decode process every API body
// from an unauthenticated remote client. The \u surrogate-pair + escape logic is the most fragile part.
// A single definition (web_stdlib.cpp includes it) → no drift. "Unstitch".
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
// Read 4 hex digits after \uXXXX (i is positioned after the 'u'); -1 if invalid.
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
// Length of the valid UTF-8 sequence that starts at s[i], or 0 if the bytes there are not one —
// Go's utf8.DecodeRune rules (RFC 3629): no overlong form, no surrogate (U+D800-DFFF), nothing above
// U+10FFFF, no truncated sequence.
inline size_t utf8_valid_len(const std::string& s, size_t i) {
    auto b = [&](size_t k) -> unsigned { return k < s.size() ? (unsigned char)s[k] : 0x100; };
    auto cont = [&](size_t k, unsigned lo = 0x80, unsigned hi = 0xBF) { unsigned c = b(k); return c >= lo && c <= hi; };
    unsigned c = b(i);
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c == 0xE0) return cont(i + 1, 0xA0) && cont(i + 2) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return cont(i + 1) && cont(i + 2) ? 3 : 0;
    if (c == 0xED) return cont(i + 1, 0x80, 0x9F) && cont(i + 2) ? 3 : 0;
    if (c == 0xF0) return cont(i + 1, 0x90) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3) return cont(i + 1) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (c == 0xF4) return cont(i + 1, 0x80, 0x8F) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    return 0;
}

// RFC 8259 / Go encoding/json rules. Returns false = invalid string (the caller rejects the whole document).
// Invalid UTF-8 in the raw text becomes U+FFFD, one per invalid byte, as Go's decoder does: a key or a
// value with a stray byte reads the same as in Go (two keys "\xff" and "\xfe" are one key, "�").
// It used to keep the raw bytes, so LOOK and a Go service in front of it could read different keys.
// OLD LOOSENESS: an unknown escape ("\x" → "x"), a broken \u ("\u12" → "\\u12"), a raw
// control character and an unterminated string were accepted — if a gateway counted the same body as invalid
// and LOOK as valid there was a mismatch (the JSON counterpart of HTTP smuggling). An unpaired surrogate
// becomes U+FFFD as in Go (it used to produce invalid UTF-8 — CESU). i: on the opening '"'.
inline bool json_decode_str(const std::string& s, size_t& i, std::string& result) {
    i++; // skip "
    result.clear();
    while (i < s.size() && s[i] != '"') {
        // A run of plain characters is validated and copied in one append; an invalid UTF-8 byte
        // becomes U+FFFD. (Copying byte by byte made a non-ASCII-heavy body ~70% slower to decode.)
        size_t run = i;
        while (i < s.size()) {
            unsigned char p = (unsigned char)s[i];
            if (p == '"' || p == '\\' || p < 0x20) break;
            if (p < 0x80) { i++; continue; }
            size_t n = utf8_valid_len(s, i);
            if (n == 0) { result.append(s, run, i - run); json_append_utf8(result, 0xFFFD); run = ++i; continue; }
            i += n;
        }
        result.append(s, run, i - run);
        if (i >= s.size() || s[i] == '"') break;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) return false;                       // raw control character
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
                i++;  // skip the 'u'
                int cp = json_read_u4(s, i);
                if (cp < 0) return false;                 // \u must be followed by 4 hex digits
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    size_t save = i;
                    int lo = -1;
                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; lo = json_read_u4(s, i); }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { i = save; cp = 0xFFFD; }       // lone high surrogate
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;                          // lone low surrogate
                }
                json_append_utf8(result, (uint32_t)cp);
                continue;  // i has already advanced
            }
            default: return false;                        // unknown escape
        }
        i++;
    }
    if (i >= s.size()) return false;                      // unterminated string
    i++; // skip closing "
    return true;
}

} // namespace look

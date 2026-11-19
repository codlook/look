#pragma once
// DKIM-Signature tag-list parser (RFC 6376 §3.2) — a PURE function, no I/O.
// "Unstitch" pattern: dkim.cpp includes this, and so does dkim_tag_test.cpp → a single definition,
// no drift, can be fuzzed/KAT'ed directly.
//
// OLD BUG (①, confirmed by two independent measurements): `dkim_hdr_raw.find(name + "=")`
// searched for the tag name as a SUBSTRING → `tag("h")` collided with the "h=" inside "bh=".
// When `bh=` came BEFORE `h=` in the header (the RFC does NOT force an order, an attacker picks the order),
// the body hash (bh=) was read instead of the signed-header list (h=) → no header
// was canonicalized → From: could end up outside the signature's coverage (a verification logic bug).
//
// RFC-COMPLIANT: tag-list = tag-spec *( ";" tag-spec ); tag-spec = tag-name "=" tag-value.
// Split on `;`, split each piece at '=', match the tag NAME EXACTLY (not as a substring).
#include <string>
#include <algorithm>

namespace look {

inline std::string dkim_tag(const std::string& dkim_header, const std::string& name) {
    // dkim_header = "DKIM-Signature: v=1; a=...; h=...; bh=...; b=..." (unfolded).
    // The value is after '=', up to the next ';'; inner whitespace (WSP/CRLF, folding) is dropped.
    size_t colon = dkim_header.find(':');
    const std::string body = (colon != std::string::npos) ? dkim_header.substr(colon + 1)
                                                           : dkim_header;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t semi = body.find(';', pos);
        std::string seg = (semi == std::string::npos) ? body.substr(pos)
                                                       : body.substr(pos, semi - pos);
        // trim the segment (including WSP/CRLF from folding)
        size_t a = seg.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            size_t b = seg.find_last_not_of(" \t\r\n");
            seg = seg.substr(a, b - a + 1);
            size_t eq = seg.find('=');
            if (eq != std::string::npos) {
                std::string tname = seg.substr(0, eq);
                // WSP around '=' is free in the RFC → trim the tag name too
                size_t ta = tname.find_first_not_of(" \t");
                if (ta != std::string::npos) {
                    size_t tb = tname.find_last_not_of(" \t");
                    tname = tname.substr(ta, tb - ta + 1);
                }
                if (tname == name) {
                    std::string val = seg.substr(eq + 1);
                    val.erase(std::remove_if(val.begin(), val.end(),
                        [](char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; }), val.end());
                    return val;
                }
            }
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
}

} // namespace look

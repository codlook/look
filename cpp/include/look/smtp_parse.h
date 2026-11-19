#pragma once
// SMTP pure parsers — "unstitch" (the pg_parse.h / dkim_tag.h pattern): separate the network-fed,
// PRE-AUTH parse logic from I/O → a single definition (no drift), fuzz + table test possible.
// smtp_server.cpp includes this header; there is NO separate .cpp/.static definition.
//
// extract_addr: extract the address from the `MAIL FROM:<addr>` / `RCPT TO:<addr>` parameter (RFC 5321 §4.1.2).
//   Past bugs (documented in the code): a bare address in `MAIL FROM:` was BROKEN, the `<>` null-sender was rejected,
//   a nested `<`/space garbage address was accepted. Now bounds-checked + strict. Network-facing → fuzz target.
#include <string>
#include <cctype>

namespace look {

// "MAIL FROM:<a@b.com>" → addr="a@b.com", true.  "<>" → addr="", true (null sender).
// Malformed (unclosed '<', nested '<', space in the address, no parameter) → false.
inline bool smtp_extract_addr(const std::string& line, std::string& addr) {
    addr.clear();
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;          // "MAIL FROM" — no parameter
    std::string rest = line.substr(colon + 1);
    size_t b = rest.find_first_not_of(" \t");
    if (b == std::string::npos) return false;              // "MAIL FROM:" — no address
    rest = rest.substr(b);

    if (rest[0] == '<') {
        size_t gt = rest.find('>');
        if (gt == std::string::npos) return false;         // unclosed angle bracket
        addr = rest.substr(1, gt - 1);
        // Nested '<' or space = malformed address (cannot be inside a path/address in the RFC)
        if (addr.find('<') != std::string::npos ||
            addr.find(' ') != std::string::npos) { addr.clear(); return false; }
        return true;                                       // addr may be empty → null sender
    }
    // Address without angle brackets (bare) — the first token before the ESMTP parameters (SIZE=, BODY=)
    size_t sp = rest.find_first_of(" \t");
    addr = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    if (addr.empty() ||
        addr.find('<') != std::string::npos ||
        addr.find('>') != std::string::npos) { addr.clear(); return false; }
    return true;
}

// Extract the verb from the command line: up to the first space/CR/LF, upper-case.
inline std::string smtp_verb(const std::string& line) {
    std::string v;
    for (char c : line) {
        if (c == ' ' || c == '\r' || c == '\n') break;
        v += (char)std::toupper((unsigned char)c);
    }
    return v;
}

} // namespace look

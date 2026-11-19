// PostgreSQL wire-protocol pure parsers — SEPARATE from the socket (fuzz + test target).
// "Unstitch": the byte-interpretation logic is here, I/O is in postgres_client.cpp. A single definition
// (postgres_client.cpp includes it) → no drift.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace look {

// Parse the ErrorResponse body: a sequence of (field-type byte, NUL-terminated value)*,
// ending with a double NUL. Returns the 'M' (human-readable message) field, "unknown error" if absent.
// Attacker-controlled bytes (the server's ERR packet) → every access is bounded by p<end.
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

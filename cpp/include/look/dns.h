#pragma once
#include <string>
#include <vector>

namespace look {

// ── DNS TXT lookup ────────────────────────────────────────────────────────────
// Returns all TXT records for the given fully-qualified domain name.
// Used by: DKIM verify (selector._domainkey.domain), SPF inbound checks.
//
// Implementation: POSIX res_query (<resolv.h>) on Linux/macOS,
//                 DnsQuery_A on Windows — zero external dependencies.
//
// Throws std::runtime_error on hard DNS errors (SERVFAIL, etc.).
// Returns empty vector on NXDOMAIN or no TXT records (not an error).

std::vector<std::string> dns_txt_lookup(const std::string& fqdn);

} // namespace look

#pragma once
#include <string>
#include <vector>

namespace look {

// ── DKIM sign (RFC 6376) ──────────────────────────────────────────────────────
// Produces a DKIM-Signature header value for prepending to the outbound message.
//
// Parameters:
//   headers      — ordered list of header name+value pairs to sign
//                  (e.g. {{"From","sender@example.com"},{"Subject","Hello"}})
//   body         — raw RFC 5322 message body (after the blank line)
//   domain       — signing domain (d= tag), e.g. "example.com"
//   selector     — DNS selector (s= tag), e.g. "mail"
//   private_key  — RSA private key in PKCS#8 PEM format
//
// Returns the complete DKIM-Signature header line (without trailing CRLF).
// Throws std::runtime_error on signing failure.
//
// Canonicalization: relaxed/relaxed (RFC 6376 §3.4) — industry default.
// Algorithm: rsa-sha256.

struct DkimHeader { std::string name; std::string value; };

std::string dkim_sign(const std::vector<DkimHeader>& headers,
                      const std::string& body,
                      const std::string& domain,
                      const std::string& selector,
                      const std::string& private_key_pem);

// ── DKIM verify ───────────────────────────────────────────────────────────────
// Verifies the DKIM-Signature header(s) in a raw RFC 5322 message.
// Returns true if at least one valid signature found.
// Uses dns_txt_lookup() to fetch the public key.
bool dkim_verify(const std::string& raw_message);

} // namespace look

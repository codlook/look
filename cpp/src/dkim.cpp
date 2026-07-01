#include "look/dkim.h"
#include "look/dns.h"
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

#ifndef OPENSSL_NO_DEPRECATED_3_0
#  define OPENSSL_NO_DEPRECATED_3_0
#endif
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/err.h>

namespace look {

// ── Base64 (RFC 4648) ─────────────────────────────────────────────────────────

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = ((unsigned int)data[i] << 16)
                       | (i+1 < len ? (unsigned int)data[i+1] << 8 : 0)
                       | (i+2 < len ? (unsigned int)data[i+2]      : 0);
        out += B64[(v >> 18) & 0x3F];
        out += B64[(v >> 12) & 0x3F];
        out += (i+1 < len) ? B64[(v >>  6) & 0x3F] : '=';
        out += (i+2 < len) ? B64[(v      ) & 0x3F] : '=';
    }
    return out;
}

static std::string b64_encode(const std::vector<unsigned char>& v) {
    return b64_encode(v.data(), v.size());
}

// ── SHA-256 digest ────────────────────────────────────────────────────────────

static std::vector<unsigned char> sha256_digest(const std::string& s) {
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), out.data());
    return out;
}

// ── Relaxed canonicalization (RFC 6376 §3.4) ─────────────────────────────────

// Header: lowercase name, unfold, compress whitespace in value
static std::string canon_header_relaxed(const std::string& name,
                                        const std::string& value) {
    std::string n = name;
    for (char& c : n) c = (char)std::tolower((unsigned char)c);

    // Unfold (remove CRLF followed by WSP)
    std::string v;
    v.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (i + 1 < value.size() && value[i] == '\r' && value[i+1] == '\n') {
            ++i; // skip \r\n
            // skip following WSP
            while (i+1 < value.size() &&
                   (value[i+1] == ' ' || value[i+1] == '\t')) ++i;
            v += ' '; continue;
        }
        v += value[i];
    }
    // Trim leading/trailing whitespace from value
    size_t s = v.find_first_not_of(" \t");
    size_t e = v.find_last_not_of(" \t");
    if (s == std::string::npos) v = "";
    else v = v.substr(s, e - s + 1);

    // Compress internal whitespace runs to single space
    std::string out;
    out.reserve(v.size());
    bool in_ws = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            if (!in_ws) { out += ' '; in_ws = true; }
        } else {
            out += c; in_ws = false;
        }
    }
    return n + ":" + out;
}

// Body: remove trailing whitespace per line, strip trailing empty lines,
// ensure single CRLF terminator
static std::string canon_body_relaxed(const std::string& body) {
    std::istringstream ss(body);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Remove trailing WSP
        size_t e = line.find_last_not_of(" \t");
        line = (e == std::string::npos) ? "" : line.substr(0, e + 1);
        lines.push_back(line);
    }
    // Strip trailing empty lines
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    // Rebuild with CRLF, add single trailing CRLF
    std::string out;
    for (auto& l : lines) out += l + "\r\n";
    if (out.empty()) out = "\r\n"; // empty body = single CRLF per RFC 6376
    return out;
}

// ── RSA-SHA256 sign via OpenSSL EVP ──────────────────────────────────────────

static std::vector<unsigned char> rsa_sha256_sign(const std::string& data,
                                                   const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) throw std::runtime_error("dkim_sign: BIO_new_mem_buf failed");

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        ERR_clear_error();
        throw std::runtime_error("dkim_sign: private key PEM parse hatası");
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pkey); throw std::runtime_error("dkim_sign: EVP_MD_CTX_new"); }

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1 ||
        EVP_DigestSignUpdate(ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); ERR_clear_error();
        throw std::runtime_error("dkim_sign: DigestSign init/update hatası");
    }

    size_t sig_len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &sig_len);
    std::vector<unsigned char> sig(sig_len);
    EVP_DigestSignFinal(ctx, sig.data(), &sig_len);
    sig.resize(sig_len);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;
}

// ── dkim_sign ─────────────────────────────────────────────────────────────────

std::string dkim_sign(const std::vector<DkimHeader>& headers,
                      const std::string& body,
                      const std::string& domain,
                      const std::string& selector,
                      const std::string& private_key_pem) {

    // 1. Canonicalize body → bh=
    std::string canon_body = canon_body_relaxed(body);
    auto bh_raw = sha256_digest(canon_body);
    std::string bh = b64_encode(bh_raw);

    // 2. Build h= tag (list of signed header names, lowercase)
    std::string h_tag;
    for (size_t i = 0; i < headers.size(); ++i) {
        std::string n = headers[i].name;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (i > 0) h_tag += ":";
        h_tag += n;
    }

    // 3. Timestamp
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                       now.time_since_epoch()).count();

    // 4. Build DKIM-Signature header without b= value (RFC 6376 §3.5)
    std::string dkim_hdr =
        "v=1; a=rsa-sha256; c=relaxed/relaxed"
        "; d=" + domain +
        "; s=" + selector +
        "; t=" + std::to_string(ts) +
        "; h=" + h_tag +
        "; bh=" + bh +
        "; b=";

    // 5. Canonicalize signed headers (in order) + DKIM-Signature header (b= empty)
    std::string data_to_sign;
    for (auto& hdr : headers) {
        data_to_sign += canon_header_relaxed(hdr.name, hdr.value) + "\r\n";
    }
    // The DKIM-Signature header itself is the last signed header (with empty b=)
    data_to_sign += canon_header_relaxed("DKIM-Signature", dkim_hdr);

    // 6. RSA-SHA256 sign
    auto sig_bytes = rsa_sha256_sign(data_to_sign, private_key_pem);
    std::string b_val = b64_encode(sig_bytes);

    // 7. Fold b= value at 72 chars (RFC 6376 §3.5 SHOULD fold)
    std::string folded;
    for (size_t i = 0; i < b_val.size(); i += 72) {
        if (i > 0) folded += "\r\n\t";
        folded += b_val.substr(i, 72);
    }

    return "DKIM-Signature: " + dkim_hdr + folded;
}

// ── dkim_verify ───────────────────────────────────────────────────────────────

bool dkim_verify(const std::string& raw_message) {
    // Parse DKIM-Signature header from raw message
    size_t sig_start = raw_message.find("DKIM-Signature:");
    if (sig_start == std::string::npos) return false;

    size_t sig_end = raw_message.find("\r\n", sig_start);
    // Unfold (continuation lines start with WSP)
    while (sig_end != std::string::npos &&
           sig_end + 2 < raw_message.size() &&
           (raw_message[sig_end+2] == ' ' || raw_message[sig_end+2] == '\t')) {
        sig_end = raw_message.find("\r\n", sig_end + 2);
    }
    std::string dkim_hdr_raw = (sig_end != std::string::npos)
        ? raw_message.substr(sig_start, sig_end - sig_start)
        : raw_message.substr(sig_start);

    // Extract tags: d=, s=, b=, bh=, h=
    auto tag = [&](const std::string& name) -> std::string {
        std::string pat = name + "=";
        size_t p = dkim_hdr_raw.find(pat);
        if (p == std::string::npos) return "";
        p += pat.size();
        size_t e = dkim_hdr_raw.find_first_of(";", p);
        std::string v = (e != std::string::npos)
            ? dkim_hdr_raw.substr(p, e - p)
            : dkim_hdr_raw.substr(p);
        // Strip whitespace
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; }),
                v.end());
        return v;
    };

    std::string domain   = tag("d");
    std::string selector = tag("s");
    std::string bh_b64   = tag("bh");
    std::string b_b64    = tag("b");

    if (domain.empty() || selector.empty() || b_b64.empty() || bh_b64.empty()) return false;

    // Verify body hash (bh=) — body starts after blank line
    {
        size_t body_start = raw_message.find("\r\n\r\n");
        std::string body = (body_start != std::string::npos)
            ? raw_message.substr(body_start + 4)
            : "";
        std::string canon_body = canon_body_relaxed(body);
        auto bh_actual = sha256_digest(canon_body);
        std::string bh_actual_b64 = b64_encode(bh_actual);
        if (bh_actual_b64 != bh_b64) return false;
    }

    // Fetch public key from DNS
    std::string dns_name = selector + "._domainkey." + domain;
    std::vector<std::string> txts;
    try { txts = dns_txt_lookup(dns_name); }
    catch (...) { return false; }

    std::string pubkey_pem;
    for (auto& t : txts) {
        size_t p = t.find("p=");
        if (p == std::string::npos) continue;
        std::string b64key = t.substr(p + 2);
        // Strip whitespace
        b64key.erase(std::remove_if(b64key.begin(), b64key.end(),
                                    [](char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'||c==';'; }),
                     b64key.end());
        if (b64key.empty()) continue;
        // Wrap in PEM
        pubkey_pem = "-----BEGIN PUBLIC KEY-----\n";
        for (size_t i = 0; i < b64key.size(); i += 64)
            pubkey_pem += b64key.substr(i, 64) + "\n";
        pubkey_pem += "-----END PUBLIC KEY-----\n";
        break;
    }
    if (pubkey_pem.empty()) return false;

    // Decode signature
    // b64_decode: use OpenSSL BIO
    std::vector<unsigned char> sig_bytes;
    {
        // Remove whitespace from b_b64
        std::string clean;
        for (char c : b_b64) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean += c;
        BIO* b64bio = BIO_new(BIO_f_base64());
        BIO_set_flags(b64bio, BIO_FLAGS_BASE64_NO_NL);
        BIO* membio = BIO_new_mem_buf(clean.data(), (int)clean.size());
        BIO* chain  = BIO_push(b64bio, membio);
        sig_bytes.resize(clean.size());
        int n = BIO_read(chain, sig_bytes.data(), (int)sig_bytes.size());
        BIO_free_all(chain);
        if (n <= 0) return false;
        sig_bytes.resize((size_t)n);
    }

    // Load public key
    BIO* bio  = BIO_new_mem_buf(pubkey_pem.data(), (int)pubkey_pem.size());
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) { ERR_clear_error(); return false; }

    // Reconstruct signed data (headers + DKIM-Signature with empty b=)
    // For a minimal but correct verify: hash the same header block
    // Strip b= value from DKIM-Signature header for verification
    std::string dkim_hdr_no_b = dkim_hdr_raw;
    size_t bp = dkim_hdr_no_b.find("b=");
    if (bp != std::string::npos) {
        size_t bv_start = bp + 2;
        size_t bv_end   = dkim_hdr_no_b.find(';', bv_start);
        dkim_hdr_no_b.replace(bv_start,
                               bv_end == std::string::npos
                                   ? dkim_hdr_no_b.size() - bv_start
                                   : bv_end - bv_start,
                               "");
    }

    // Parse h= to get signed header names
    std::string h_val = tag("h");
    std::vector<std::string> signed_hdrs;
    {
        std::istringstream ss(h_val);
        std::string tok;
        while (std::getline(ss, tok, ':')) {
            for (char& c : tok) c = (char)std::tolower((unsigned char)c);
            signed_hdrs.push_back(tok);
        }
    }

    // Find each signed header in the raw message (before blank line)
    size_t hdr_end = raw_message.find("\r\n\r\n");
    std::string hdr_block = (hdr_end != std::string::npos)
        ? raw_message.substr(0, hdr_end)
        : raw_message;

    // RFC 6376 §5.4.2: for duplicate header names, take from bottom (rfind).
    // Each occurrence in h= consumes one instance starting from the bottom.
    // We track consumed positions so the second occurrence gets the next one up.
    std::string hdr_lower = hdr_block;
    for (char& c : hdr_lower) c = (char)std::tolower((unsigned char)c);

    // For each name, keep a "search ceiling" (start from end, move up on repeats)
    std::unordered_map<std::string, size_t> search_from;

    std::string data_to_verify;
    for (auto& hname : signed_hdrs) {
        std::string search = hname + ":";
        size_t ceiling = search_from.count(hname)
            ? search_from[hname]
            : hdr_lower.size();
        // rfind up to ceiling
        size_t p = hdr_lower.rfind(search, ceiling > 0 ? ceiling - 1 : 0);
        if (p == std::string::npos) continue;
        search_from[hname] = p; // next duplicate must be above this position
        size_t e = hdr_block.find("\r\n", p);
        // Unfold continuation lines
        while (e != std::string::npos && e + 2 < hdr_block.size() &&
               (hdr_block[e+2] == ' ' || hdr_block[e+2] == '\t'))
            e = hdr_block.find("\r\n", e + 2);
        std::string hval = (e != std::string::npos)
            ? hdr_block.substr(p + hname.size() + 1, e - p - hname.size() - 1)
            : hdr_block.substr(p + hname.size() + 1);
        data_to_verify += canon_header_relaxed(hname, hval) + "\r\n";
    }
    data_to_verify += canon_header_relaxed("DKIM-Signature",
                          dkim_hdr_no_b.substr(dkim_hdr_no_b.find(':') + 1));

    // Verify
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx &&
        EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(ctx, data_to_verify.data(), data_to_verify.size()) == 1) {
        ok = (EVP_DigestVerifyFinal(ctx, sig_bytes.data(), sig_bytes.size()) == 1);
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ERR_clear_error();
    return ok;
}

} // namespace look

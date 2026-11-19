#include "look/web.h"
#ifndef _WIN32
extern char** environ;   // CGI request headers (HTTP_*) — init_from_cgi
#endif
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <iomanip>
#include <stdexcept>
#include <filesystem>
#ifndef _WIN32
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#endif
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   // must come BEFORE bcrypt.h (for the BCRYPT types)
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace look_internal {

// ── SHA-256 (standalone, zero deps) ──────────────────────────────────────────

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32-n)); }

static std::string sha256(const std::string& data) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    std::vector<uint8_t> msg(data.begin(), data.end());
    uint64_t bit_len = (uint64_t)data.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bit_len >> (i*8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)msg[off+i*4]<<24)|((uint32_t)msg[off+i*4+1]<<16)|
                   ((uint32_t)msg[off+i*4+2]<<8)|(uint32_t)msg[off+i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t temp1=hh+S1+ch+K256[i]+w[i];
            uint32_t S0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t temp2=S0+maj;
            hh=g; g=f; f=e; e=d+temp1;
            d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i)
        oss << std::hex << std::setfill('0') << std::setw(8) << h[i];
    return oss.str();
}

// ── Magic Byte → MIME ─────────────────────────────────────────────────────────

static std::string detect_mime(const std::string& data) {
    auto starts = [&](std::initializer_list<uint8_t> sig) {
        if (data.size() < sig.size()) return false;
        size_t i = 0;
        for (auto b : sig) if ((uint8_t)data[i++] != b) return false;
        return true;
    };
    if (starts({0xFF,0xD8,0xFF}))                   return "image/jpeg";
    if (starts({0x89,0x50,0x4E,0x47,0x0D,0x0A}))   return "image/png";
    if (starts({0x47,0x49,0x46,0x38}))              return "image/gif";
    if (starts({0x52,0x49,0x46,0x46}) &&
        data.size()>=12 && data.substr(8,4)=="WEBP") return "image/webp";
    if (starts({0x25,0x50,0x44,0x46}))              return "application/pdf";
    if (starts({0x50,0x4B,0x03,0x04}))              return "application/zip";
    if (starts({0x1F,0x8B}))                        return "application/gzip";
    // SVG — text based, check content
    if (data.find("<svg") != std::string::npos ||
        data.find("<?xml") != std::string::npos)    return "image/svg+xml";
    // Plain text — NO magic bytes (completes the SVG content-check pattern, dogfooding #4):
    // text/csv/log without magic bytes could not match allow_mime:["text/plain"] (it returned octet-stream).
    // Conservative heuristic: non-empty + no NUL + no C0 control character (except TAB/CR/LF).
    // Binary formats were already caught above; binary with NUL/control characters stays octet-stream.
    if (!data.empty()) {
        bool is_text = true;
        for (unsigned char c : data) {
            if (c == 0x00) { is_text = false; break; }                    // NUL → binary
            if (c < 0x09 || (c > 0x0D && c < 0x20)) { is_text = false; break; } // C0 kontrol → binary
        }
        if (is_text) return "text/plain";
    }
    return "application/octet-stream";
}

// ── Random hex filename ───────────────────────────────────────────────────────

// CSPRNG-based temp file name (rand() removed — fix for predictable randomness)
static std::string random_hex(int bytes = 16) {
    std::vector<unsigned char> buf(bytes);
    // Fail-closed: if the CSPRNG fails, throw an error instead of generating a predictable name from a zeroed buffer.
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf.data(), (ULONG)bytes,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("random_hex: CSPRNG (BCryptGenRandom) failed");
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) throw std::runtime_error("random_hex: could not open /dev/urandom");
    int got = 0;
    while (got < bytes) {
        ssize_t n = read(fd, buf.data() + got, bytes - got);
        if (n <= 0) { close(fd); throw std::runtime_error("random_hex: /dev/urandom read error"); }
        got += (int)n;
    }
    close(fd);
#endif
    std::ostringstream oss;
    for (int i = 0; i < bytes; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
    return oss.str();
}

// ── MIME → extension ─────────────────────────────────────────────────────────

static std::string mime_to_ext(const std::string& mime) {
    if (mime == "image/jpeg")            return ".jpg";
    if (mime == "image/png")             return ".png";
    if (mime == "image/gif")             return ".gif";
    if (mime == "image/webp")            return ".webp";
    if (mime == "image/svg+xml")         return ".svg";
    if (mime == "application/pdf")       return ".pdf";
    if (mime == "application/zip")       return ".zip";
    if (mime == "application/gzip")      return ".gz";
    return ".bin";
}

} // namespace look_internal

namespace look {

#ifdef _WIN32
static char** look_environ() { return _environ; }
#else
static char** look_environ() { return ::environ; }
#endif

void WebContext::init_from_cgi() {
    // Method
    const char* m = std::getenv("REQUEST_METHOD");
    method = m ? m : "GET";

    // Path resolution — two modes:
    //
    // 1. Apache Action (CGI): PATH_INFO = /index.lk/menu/route
    //    strip the .lk prefix; the rest is the route path
    //
    // 2. mod_proxy_fcgi RewriteRule [P] (FastCGI): PATH_INFO empty,
    //    REQUEST_URI = /menu/route (query string not included)
    //    SCRIPT_NAME = /index.lk (the target we added)
    //    Real route = REQUEST_URI minus the SCRIPT_NAME prefix (if any)
    {
        const char* pi_env = std::getenv("PATH_INFO");
        std::string pi_str = pi_env ? pi_env : "";

        if (!pi_str.empty()) {
            // Mode 1: CGI Action — strip .lk if present in PATH_INFO
            size_t lk = pi_str.find(".lk");
            if (lk != std::string::npos) {
                size_t slash = pi_str.find('/', lk + 3);
                path = (slash != std::string::npos) ? pi_str.substr(slash) : "/";
            } else {
                path = pi_str;
            }
        } else {
            // Mode 2: FastCGI (mod_proxy_fcgi RewriteRule [P]) — use REQUEST_URI
            // REQUEST_URI = /menu/burger-cafe (original URL, correct for routing)
            // SCRIPT_NAME = assigned by Apache: strip if /index.lk, otherwise do not use
            const char* ru = std::getenv("REQUEST_URI");
            if (ru && *ru) {
                std::string uri(ru);
                // Cut off the query string (?...) — path only
                size_t q = uri.find('?');
                if (q != std::string::npos) uri = uri.substr(0, q);
                // Strip only if SCRIPT_NAME ends with .lk like /index.lk
                // Do not strip route paths like /menu/burger-cafe
                const char* sn = std::getenv("SCRIPT_NAME");
                if (sn && *sn) {
                    std::string script_name(sn);
                    bool sn_is_lk = script_name.size() > 3 &&
                                    script_name.substr(script_name.size()-3) == ".lk";
                    if (sn_is_lk && uri.rfind(script_name, 0) == 0)
                        uri = uri.substr(script_name.size());
                }
                path = uri.empty() ? "/" : uri;
            } else {
                path = "/";
            }
        }
    }

    // Query string
    const char* qs = std::getenv("QUERY_STRING");
    query_string = qs ? qs : "";
    get_params = parse_query(query_string);

    // Body (POST)
    const char* cl = std::getenv("CONTENT_LENGTH");
    if (cl && *cl) {
        int len = std::atoi(cl);
        if (len > 0 && len < 10 * 1024 * 1024) { // max 10MB
            body.resize(len);
            std::cin.read(&body[0], len);
        }
    }

    // POST params
    const char* ct = std::getenv("CONTENT_TYPE");
    content_type = ct ? ct : "";
    parse_post_body();

    // Remote addr
    const char* ra = std::getenv("REMOTE_ADDR");
    remote_addr = ra ? ra : "";
    { const char* dr = std::getenv("DOCUMENT_ROOT"); document_root = dr ? dr : ""; }

    // HTTPS and Host: the web server sets up the CGI environment (not the client) — same rule as FastCGI.
    // Previously is_https was never set in CGI: on an HTTPS site the session cookie did not get Secure.
    { const char* h = std::getenv("HTTPS"); std::string hs = h ? h : "";
      for (auto& c : hs) c = (char)std::tolower((unsigned char)c);
      const char* rs = std::getenv("REQUEST_SCHEME");
      is_https = (hs == "on" || hs == "1" || (rs && std::string(rs) == "https")); }
    { const char* hh = std::getenv("HTTP_HOST"); host = clean_host(hh ? hh : ""); }

    // Request headers: the web server passes each header as an HTTP_<NAME> environment variable (RFC 3875
    // 4.1.18) — same conversion as the HTTP_* parameters in FastCGI. Previously headers_in was never
    // filled in CGI: request::header() was always null, X-CSRF-Token and Origin were not visible.
    for (char** e = look_environ(); e && *e; ++e) {
        std::string kv = *e;
        auto eq = kv.find('=');
        if (eq == std::string::npos || eq <= 5 || kv.compare(0, 5, "HTTP_") != 0) continue;
        std::string name = kv.substr(5, eq - 5);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return c == '_' ? '-' : (char)std::tolower(c); });
        headers_in[name] = kv.substr(eq + 1);
    }

    // Cookies
    const char* ck = std::getenv("HTTP_COOKIE");
    if (ck) cookies_in = parse_cookies(ck);
}

std::string WebContext::url_decode(const std::string& s, bool plus_is_space) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            result += plus_is_space ? ' ' : '+';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            try {
                int h = std::stoi(s.substr(i + 1, 2), nullptr, 16);
                result += (char)h;
                i += 2;
            } catch (...) {
                result += s[i];  // invalid %xx → leave a literal '%'
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

std::map<std::string, std::string> WebContext::parse_query(const std::string& qs) {
    std::map<std::string, std::string> result;
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            result[url_decode(pair)] = "";
        } else {
            result[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
    }
    return result;
}

std::string WebContext::clean_host(const std::string& raw) {
    // Host comes from the client: used in the Origin comparison and in connections the application builds.
    // Only name[:port] / [ipv6][:port] is accepted; anything else is "" (ignored).
    std::string h = raw;
    h.erase(0, h.find_first_not_of(" \t"));
    auto last = h.find_last_not_of(" \t"); h.erase(last == std::string::npos ? 0 : last + 1);
    if (h.empty() || h.size() > 255) return "";
    size_t i = 0;
    if (h[0] == '[') {                                   // [ipv6]
        auto close = h.find(']');
        if (close == std::string::npos || close < 2) return "";
        for (size_t j = 1; j < close; ++j) {
            char c = h[j];
            if (!(std::isxdigit((unsigned char)c) || c == ':' || c == '.')) return "";
        }
        i = close + 1;
    } else {
        for (; i < h.size() && h[i] != ':'; ++i) {
            char c = h[i];
            if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_')) return "";
        }
        if (i == 0) return "";
    }
    if (i < h.size()) {                                  // :port
        if (h[i] != ':' || i + 1 == h.size() || h.size() - i - 1 > 5) return "";
        for (size_t j = i + 1; j < h.size(); ++j) if (!std::isdigit((unsigned char)h[j])) return "";
    }
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    return h;
}

std::map<std::string, std::string> WebContext::parse_cookies(const std::string& raw) {
    std::map<std::string, std::string> result;
    std::istringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ';')) {
        // trim
        size_t s = item.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        item = item.substr(s);
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        result[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return result;
}

void WebContext::set_status(int code) {
    status_code = code;
    switch (code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 422: status_text = "Unprocessable Entity"; break;
        case 500: status_text = "Internal Server Error"; break;
        default:  status_text = "Unknown"; break;
    }
}

// POST body — by content_type. SINGLE source; the FastCGI, --mode http and CGI
// entry points all call this (see web.h).
// Content-Type → media type (lowercase) + parameters (name lowercase; value a token or quoted
// string, \ escapes decoded). Go mime.ParseMediaType model. false = malformed syntax.
static bool parse_media_type(const std::string& ct, std::string& type,
                             std::map<std::string, std::string>& params) {
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    auto trim = [](const std::string& s) {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    size_t semi = ct.find(';');
    type = lower(trim(ct.substr(0, semi)));
    if (type.empty()) return false;
    size_t i = (semi == std::string::npos) ? ct.size() : semi + 1;
    while (i < ct.size()) {
        while (i < ct.size() && (ct[i] == ' ' || ct[i] == '\t' || ct[i] == ';')) i++;
        if (i >= ct.size()) break;
        size_t ks = i;
        while (i < ct.size() && ct[i] != '=' && ct[i] != ';') i++;
        if (i >= ct.size() || ct[i] != '=') return false;            // parameter without a value
        std::string key = lower(trim(ct.substr(ks, i - ks)));
        i++;
        std::string val;
        if (i < ct.size() && ct[i] == '"') {
            i++;
            bool closed = false;
            while (i < ct.size()) {
                if (ct[i] == '\\' && i + 1 < ct.size()) { val.push_back(ct[i + 1]); i += 2; continue; }
                if (ct[i] == '"') { closed = true; i++; break; }
                val.push_back(ct[i++]);
            }
            if (!closed) return false;
        } else {
            size_t vs = i;
            while (i < ct.size() && ct[i] != ';') i++;
            val = trim(ct.substr(vs, i - vs));
        }
        if (key.empty() || params.count(key)) return false;           // empty or duplicate name
        params[key] = val;
    }
    return true;
}

// RFC 2046 §5.1.1: boundary is 1-70 characters, bchars set, no trailing space.
static bool multipart_boundary_ok(const std::string& b) {
    if (b.empty() || b.size() > 70 || b.back() == ' ') return false;
    for (unsigned char c : b) {
        if (std::isalnum(c)) continue;
        switch (c) { case '\'': case '(': case ')': case '+': case '_': case ',': case '-': case '.':
                     case '/': case ':': case '=': case '?': case ' ': continue; default: return false; }
    }
    return true;
}

void WebContext::parse_post_body() {
    // The media type must match EXACTLY (Go). Previously a substring was searched and the boundary was cut raw:
    // bodies with boundary="B" (quoted — .NET MultipartFormDataContent, some Java clients),
    // "boundary=B; charset=utf-8" and "BOUNDARY=B" had ALL their fields SILENTLY lost.
    std::string type;
    std::map<std::string, std::string> params;
    if (!parse_media_type(content_type, type, params)) return;   // body with an unreadable type: not parsed
    if (type == "application/x-www-form-urlencoded") {
        post_params = parse_query(body);
    } else if (type == "multipart/form-data") {
        auto b = params.find("boundary");
        if (b == params.end() || !multipart_boundary_ok(b->second)) {
            body_error_status = 400;
            body_error = "multipart/form-data without a valid boundary parameter";
            return;
        }
        parse_multipart(b->second);
    }
}

void WebContext::parse_multipart(const std::string& boundary) {
    // RFC 2046 multipart — delimiter: "--" + boundary
    std::string delim = "--" + boundary;
    std::string end_delim = delim + "--";

    size_t pos = body.find(delim);
    if (pos == std::string::npos) return;

    // Part count cap — a 10MB body can hold ~250K parts of ~40-byte file parts
    // → each one costs SHA-256 + a temp file = disk/CPU amplification DoS.
    int part_count = 0;
    static constexpr int MAX_PARTS = 1000;

    while (true) {
        pos += delim.size();
        if (pos + 2 <= body.size() && body.compare(pos, 2, "--") == 0) break;   // closing delimiter
        // Explicit error when the limit is exceeded (413; Go "multipart: message too large"). Previously the loop broke and
        // the remaining parts were SILENTLY dropped (_csrf or data in the 1001st field was lost). The counter is
        // checked AFTER the closing delimiter: exactly 1000 parts are valid.
        if (++part_count > MAX_PARTS) {
            body_error_status = 413;
            body_error = "multipart/form-data with more than " + std::to_string(MAX_PARTS) + " parts";
            break;
        }
        if (pos < body.size() && body[pos] == '\r') pos++;
        if (pos < body.size() && body[pos] == '\n') pos++;

        // Read the header section (up to \r\n\r\n)
        size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) break;
        std::string raw_headers = body.substr(pos, header_end - pos);
        pos = header_end + 4;

        // Parse the Content-Disposition header
        std::string field_name, filename;
        {
            std::istringstream hs(raw_headers);
            std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // Content-Disposition: form-data; name="field"; filename="file.jpg"
                // Header name is case-insensitive (RFC 7578 → RFC 2183/822; Go textproto). Previously
                // fields from a client writing "content-disposition:" were ignored.
                static const std::string CD = "content-disposition:";
                if (line.size() >= CD.size() &&
                    std::equal(CD.begin(), CD.end(), line.begin(),
                               [](char a, char b) { return a == std::tolower((unsigned char)b); })) {
                    // Parameters are separated by ';', order is free (RFC 7578), the value may be quoted.
                    // Previously "name=\"" was searched as plain text: with the order `filename="a.jpg"; name="avatar"`
                    // the field name came out "a.jpg" (the name= inside filename= matched).
                    auto extract = [&](const std::string& key) -> std::string {
                        size_t i = line.find(':');
                        if (i == std::string::npos) return {};
                        size_t n = line.size();
                        i++;
                        while (i < n) {
                            while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == ';')) i++;
                            size_t ks = i;
                            while (i < n && line[i] != '=' && line[i] != ';') i++;
                            std::string k = line.substr(ks, i - ks);
                            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                            std::string v;
                            if (i < n && line[i] == '=') {
                                i++;
                                while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
                                if (i < n && line[i] == '"') {
                                    i++;
                                    while (i < n && line[i] != '"') {
                                        if (line[i] == '\\' && i + 1 < n) i++;   // \" and \\ escapes
                                        v.push_back(line[i++]);
                                    }
                                    if (i < n) i++;   // closing quote
                                    while (i < n && line[i] != ';') i++;
                                } else {
                                    size_t vs = i;
                                    while (i < n && line[i] != ';') i++;
                                    v = line.substr(vs, i - vs);
                                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                                }
                            }
                            if (k.size() == key.size() &&
                                std::equal(k.begin(), k.end(), key.begin(),
                                           [](char a, char b) { return std::tolower((unsigned char)a) == b; }))
                                return v;
                        }
                        return {};
                    };
                    field_name = extract("name");
                    filename   = extract("filename");
                }
            }
        }

        // Find the part body (up to the next delimiter)
        size_t part_end = body.find("\r\n" + delim, pos);
        if (part_end == std::string::npos) break;
        std::string part_data = body.substr(pos, part_end - pos);
        pos = part_end + 2;

        if (!filename.empty() && !field_name.empty()) {
            // File part — write to a temp file
            UploadedFile uf;
            uf.field_name = field_name;
            // Client filename is UNTRUSTED: take the basename (some browsers send a
            // full path), drop control chars, cap length. Metadata only — the stored
            // file is named by sha256; consumers must still escape it for display.
            {
                std::string fn = filename;
                size_t slash = fn.find_last_of("/\\");
                if (slash != std::string::npos) fn = fn.substr(slash + 1);
                std::string clean;
                for (char ch : fn) { if ((unsigned char)ch >= 0x20 && ch != 0x7f) clean.push_back(ch); }
                if (clean.size() > 255) clean.resize(255);
                uf.orig_name = clean;
            }
            uf.size       = part_data.size();
            uf.mime       = look_internal::detect_mime(part_data);
            uf.sha256     = look_internal::sha256(part_data);

            // Temp file path
            std::string tmp_dir;
            const char* td = std::getenv("TEMP");
            if (!td) td = std::getenv("TMP");
            if (!td) td = "/tmp";
            tmp_dir = td;
            std::string ext = look_internal::mime_to_ext(uf.mime);
            std::string tmp_name = look_internal::random_hex(16) + ext;
            uf.temp_path = tmp_dir + "/" + tmp_name;

            // Write to disk — only the owner can read (0600) and an existing file is not overwritten
            // (O_EXCL: does not follow a pre-planted symlink). Previously 0644 depending on umask: in a shared
            // /tmp every user on the machine could read the uploaded files.
            bool wrote = false;
#ifndef _WIN32
            {
                int wfd = ::open(uf.temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (wfd >= 0) {
                    size_t off = 0;
                    while (off < part_data.size()) {
                        ssize_t w = ::write(wfd, part_data.data() + off, part_data.size() - off);
                        if (w < 0) { if (errno == EINTR) continue; break; }
                        off += (size_t)w;
                    }
                    wrote = off == part_data.size();
                    ::close(wfd);
                    if (!wrote) ::unlink(uf.temp_path.c_str());
                }
            }
#else
            {
                std::ofstream out(uf.temp_path, std::ios::binary);
                if (out) { out.write(part_data.data(), (std::streamsize)part_data.size()); wrote = (bool)out; }
            }
#endif
            if (wrote) {
                uf.valid = true;
                // RAII cleanup: a temp file that is not consumed is deleted when WebContext dies.
                uf.temp_cleanup = std::make_shared<TempFileGuard>();
                uf.temp_cleanup->path = uf.temp_path;
            }
            uploaded_files[field_name] = std::move(uf);
        } else if (!field_name.empty()) {
            // Normal form field
            post_params[field_name] = part_data;
        }

        // Find the next delimiter
        size_t next = body.find(delim, pos);
        if (next == std::string::npos) break;
        pos = next;
    }
}

std::string WebContext::build_headers() const {
    std::string out = "Status: " + std::to_string(status_code) + " " + status_text + "\r\n";
    // Content-Type default
    bool has_ct = false;
    // Defense-in-depth: truncate a header name/value at CR/LF when writing — response splitting
    // is prevented (the input layer also sanitizes; this is the last line of defense).
    auto strip_crlf = [](const std::string& s) {
        size_t cut = s.find_first_of("\r\n");
        return cut == std::string::npos ? s : s.substr(0, cut);
    };
    for (auto& [k, v] : headers_out) {
        out += strip_crlf(k) + ": " + strip_crlf(v) + "\r\n";
        if (k == "Content-Type") has_ct = true;
    }
    // Each cookie is a separate Set-Cookie line (a std::map cannot hold the same key more than once)
    for (auto& c : set_cookies_out)
        out += "Set-Cookie: " + strip_crlf(c) + "\r\n";
    if (!has_ct)
        out += "Content-Type: text/html; charset=utf-8\r\n";
    out += "\r\n";
    return out;
}

// ── svg_sanitize — single source of truth ────────────────────────────────────

static std::string svg_attr_lower(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (unsigned char c : s)
        if (!std::isspace(c)) v += (char)std::tolower(c);
    return v;
}

static bool svg_value_dangerous(const std::string& raw) {
    const std::string v = svg_attr_lower(raw);
    if (v.find("javascript:") != std::string::npos) return true;
    if (v.find("vbscript:")   != std::string::npos) return true;
    if (v.find("data:")       != std::string::npos) return true;
    if (v.find("expression(") != std::string::npos) return true;
    for (const char* scheme : {"javascript:", "data:", "vbscript:"}) {
        std::string bare = std::string("url(")  + scheme;
        std::string sq   = std::string("url('") + scheme;
        std::string dq   = std::string("url(\"") + scheme;
        if (v.find(bare) != std::string::npos) return true;
        if (v.find(sq)   != std::string::npos) return true;
        if (v.find(dq)   != std::string::npos) return true;
    }
    return false;
}

std::string svg_sanitize(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    const size_t n = input.size();
    size_t i = 0;

    while (i < n) {
        if (input[i] != '<') { out += input[i++]; continue; }

        size_t tag_end = input.find('>', i);
        if (tag_end == std::string::npos) { out += input[i++]; continue; }

        const std::string tag = input.substr(i + 1, tag_end - i - 1);
        if (tag.empty())   { i = tag_end + 1; continue; }
        if (tag[0] == '!') { i = tag_end + 1; continue; }

        const bool closing = (tag[0] == '/');
        const std::string_view body_sv = closing
            ? std::string_view(tag).substr(1)
            : std::string_view(tag);

        size_t ne = 0;
        while (ne < body_sv.size() &&
               !std::isspace((unsigned char)body_sv[ne]) &&
               body_sv[ne] != '/' && body_sv[ne] != '>') ++ne;
        const std::string elem_raw(body_sv.substr(0, ne));
        std::string elem;
        for (char c : elem_raw) elem += (char)std::tolower(c);

        if (SVG_SAFE_ELEMENTS.find(elem) == SVG_SAFE_ELEMENTS.end()) {
            i = tag_end + 1;
            if (!closing && !tag.empty() && tag.back() != '/') {
                const std::string close_marker = "</" + elem_raw;
                size_t cp = input.find(close_marker, i);
                if (cp != std::string::npos) {
                    size_t ce = input.find('>', cp);
                    i = (ce != std::string::npos) ? ce + 1 : n;
                }
            }
            continue;
        }

        const bool is_use = (elem == "use");
        std::string safe = (closing ? "</" : "<") + elem_raw;
        std::string_view attrs = body_sv.substr(ne);
        size_t ai = 0;

        while (ai < attrs.size()) {
            while (ai < attrs.size() && std::isspace((unsigned char)attrs[ai])) ++ai;
            if (ai >= attrs.size() || attrs[ai] == '/' || attrs[ai] == '>') break;

            size_t an0 = ai;
            while (ai < attrs.size() &&
                   attrs[ai] != '=' &&
                   !std::isspace((unsigned char)attrs[ai]) &&
                   attrs[ai] != '/' && attrs[ai] != '>') ++ai;
            const std::string attr_name(attrs.substr(an0, ai - an0));
            std::string attr_lower;
            for (char c : attr_name) attr_lower += (char)std::tolower(c);

            std::string attr_val;
            if (ai < attrs.size() && attrs[ai] == '=') {
                ++ai;
                if (ai < attrs.size() && (attrs[ai] == '"' || attrs[ai] == '\'')) {
                    char q = attrs[ai++];
                    size_t vs = ai;
                    while (ai < attrs.size() && attrs[ai] != q) ++ai;
                    attr_val = std::string(attrs.substr(vs, ai - vs));
                    if (ai < attrs.size()) ++ai;
                } else {
                    size_t vs = ai;
                    while (ai < attrs.size() &&
                           !std::isspace((unsigned char)attrs[ai]) &&
                           attrs[ai] != '>') ++ai;
                    attr_val = std::string(attrs.substr(vs, ai - vs));
                }
            }

            if (attr_lower.size() >= 2 && attr_lower.substr(0, 2) == "on") continue;
            if (svg_value_dangerous(attr_val)) continue;
            if (is_use && (attr_lower == "href" || attr_lower == "xlink:href"))
                if (attr_val.empty() || attr_val[0] != '#') continue;

            safe += ' ' + attr_name + "=\"" + attr_val + '"';
        }

        if (!tag.empty() && tag.back() == '/') safe += '/';
        safe += '>';
        out += safe;
        i = tag_end + 1;
    }
    return out;
}

} // namespace look

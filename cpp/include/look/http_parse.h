// HTTP/1.1 request/header pure parser — SEPARATE from the socket (fuzz + test target).
// "Unstitch": the logic that interprets a raw request buffer → HttpRequest lives here; I/O (recv,
// timeout, chunked body) is in http_server.cpp. A single definition (http_server.cpp includes it)
// → no drift. This surface is PRE-AUTH: anyone on the network feeds these bytes.
//
// Reference: the behaviour of the Go net/http (1.27) server — the same raw requests were sent to Go and to LOOK
// and measured (tests/http_rfc9112_test.py table). Wherever we were LOOSER than Go we were
// brought to Go (double Host, 1.1 without Host, header name/value validation, header limit, version,
// request target, response to a rejected request). Where we are STRICTER than Go (bare LF, obs-fold, CL+TE,
// chunk line) it is request smuggling defence → stays strict, only now a 400 response is written.
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>
#include "look/http_server.h"

namespace look {

// Go: MaxHeaderBytes default is 1 MB (+4096 margin); more than 500 header fields gives 431 (measured).
constexpr size_t HTTP_MAX_HEADER_BYTES  = 1024 * 1024 + 4096;
constexpr size_t HTTP_MAX_HEADER_FIELDS = 500;

// Character classes — a 256-entry table (one read per byte; hot path: every header byte of every request).
//   1 = tchar (RFC 9110 §5.6.2: header name, method)
//   2 = field value byte (no CTL except HTAB; ≥0x80 free — Go)
//   4 = Host byte (the Go httpguts.ValidHostHeader set)
struct HttpCharTable {
    unsigned char t[256];
    constexpr HttpCharTable() : t{} {
        for (int c = 0; c < 256; ++c) {
            bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            unsigned char v = 0;
            if (alnum) v |= 1 | 4;
            if (!((c < 0x20 && c != '\t') || c == 0x7f)) v |= 2;
            t[c] = v;
        }
        for (char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'})
            t[(unsigned char)c] |= 1;
        for (char c : {'!', '$', '%', '&', '(', ')', '*', '+', ',', '-', '.', ':', ';', '=', '[', '\'', ']', '_', '~'})
            t[(unsigned char)c] |= 4;
    }
};
inline constexpr HttpCharTable HTTP_CHARS{};

inline bool http_tchar(unsigned char c) { return HTTP_CHARS.t[c] & 1; }
inline bool http_span_is(const char* p, size_t n, unsigned char cls) {
    for (size_t i = 0; i < n; ++i) if (!(HTTP_CHARS.t[(unsigned char)p[i]] & cls)) return false;
    return true;
}
inline bool http_token(const std::string& s) { return !s.empty() && http_span_is(s.data(), s.size(), 1); }
// Field value: no control character other than HTAB (NUL, CR, LF, DEL…). ≥0x80 free (Go).
inline bool http_field_value_ok(const std::string& v) { return http_span_is(v.data(), v.size(), 2); }
// Host value: the Go httpguts.ValidHostHeader character set (an empty value is valid).
inline bool http_host_ok(const std::string& h) { return http_span_is(h.data(), h.size(), 4); }

inline const char* http_status_reason(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 431: return "Request Header Fields Too Large";
        case 505: return "HTTP Version Not Supported";
        default:  return "Bad Request";
    }
}

// Parse the raw request bytes (request-line + header block, up to "\r\n\r\n").
// Reading the body belongs to the caller (Content-Length / Transfer-Encoding).
// Returns 0 = valid; otherwise the response status code (400 / 431 / 505) — the caller writes that response and
// closes the connection (it used to close without a response: the client only saw "connection lost").
// Accesses are attacker-controlled.
inline int http_parse_request(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return 400;
    if (header_end + 4 > HTTP_MAX_HEADER_BYTES) return 431;

    // RFC 9112 §2.2 STRICT CRLF: a bare \n or \r OUTSIDE a \r\n pair in the header block → 400
    // (request smuggling: the parser splits at \r\n, a bare \n is kept in the value → an embedded second CL/TE
    // is swallowed; an LF-tolerant front-end counts it as a separate header → body-framing desync). Go counts a bare
    // LF as a line end; we stay strict. A separate full scan is NOT NEEDED: since lines are
    // split at "\r\n", a bare CR/LF can only remain INSIDE the method, target, version, name or value, and
    // the validation of each rejects CR/LF (method/name = tchar, target > 0x20, version fixed
    // format, value = no CTL). A separate loop block used to scan from start to end once more.

    // ── Request line: method SP request-target SP HTTP-version (Go: a single space separator) ──
    // A leading blank line (CRLF) is 400 as in Go: the request line is empty.
    size_t line_end = raw.find("\r\n");
    const std::string rl = raw.substr(0, line_end);
    size_t s1 = rl.find(' ');
    if (s1 == std::string::npos) return 400;
    size_t s2 = rl.find(' ', s1 + 1);
    if (s2 == std::string::npos) return 400;
    req.method = rl.substr(0, s1);
    std::string target = rl.substr(s1 + 1, s2 - s1 - 1);
    std::string version = rl.substr(s2 + 1);

    // Version: exactly "HTTP/d.d" (Go ParseHTTPVersion); anything other than 1.x → 505.
    if (version.size() != 8 || version.compare(0, 5, "HTTP/") != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' || version[7] < '0' || version[7] > '9')
        return 400;
    if (version[5] != '1') return 505;
    // 1.1 and later 1.x mean the same (Go ProtoAtLeast(1,1)): keep-alive default, Host requirement.
    req.version = (version[7] == '0') ? "HTTP/1.0" : "HTTP/1.1";
    const bool http11 = (req.version == "HTTP/1.1");

    if (!http_token(req.method)) return 400;
    if (target.empty()) return 400;
    for (unsigned char c : target) if (c < 0x21 || c == 0x7f) return 400;   // CTL / space

    // ── Request-target forms (RFC 9112 §3.2) ────────────────────────────────────
    std::string abs_authority;   // the authority part of absolute-form (replaces Host)
    bool absolute_form = false;
    if (target[0] == '/') {
        // origin-form
    } else if (target == "*") {
        if (req.method != "OPTIONS") return 400;          // asterisk-form only for OPTIONS
    } else if (req.method == "CONNECT") {
        // authority-form — LOOK is not a proxy; the route does not match (404), Go also passes it to the handler
    } else {
        // absolute-form: scheme "://" authority [path]
        size_t se = target.find("://");
        if (se == std::string::npos || se == 0) return 400;
        for (size_t i = 0; i < se; ++i) {
            unsigned char c = (unsigned char)target[i];
            bool ok = std::isalpha(c) || (i > 0 && (std::isdigit(c) || c == '+' || c == '-' || c == '.'));
            if (!ok) return 400;
        }
        std::string rest = target.substr(se + 3);
        size_t pe = rest.find_first_of("/?");
        abs_authority = rest.substr(0, pe);
        size_t at = abs_authority.rfind('@');
        if (at != std::string::npos) abs_authority = abs_authority.substr(at + 1);
        if (!http_host_ok(abs_authority)) return 400;
        target = (pe == std::string::npos) ? "/" : rest.substr(pe);
        if (target[0] == '?') target = "/" + target;
        absolute_form = true;
    }

    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        req.path         = target.substr(0, qpos);
        req.query_string = target.substr(qpos + 1);
    } else {
        req.path = target;
    }

    // ── Header fields ───────────────────────────────────────────────────────────
    size_t pos = line_end + 2;
    size_t fields = 0;
    bool have_host = false;
    const char* d = raw.data();
    while (pos < header_end + 2) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) break;
        if (++fields > HTTP_MAX_HEADER_FIELDS) return 431;
        const size_t ls = pos, le = end;   // line [ls, le) — no intermediate copy (hot path)
        pos = end + 2;
        // RFC 9112 §5.2: obs-fold (a continuation line starting with SP/HTAB) is FORBIDDEN in a request. If a strict
        // front-end resolves a folded framing header (CL/TE) differently there is desync. Go accepts it;
        // we stay strict.
        if (ls < le && (d[ls] == ' ' || d[ls] == '\t')) return 400;
        const void* cp = std::memchr(d + ls, ':', le - ls);
        if (!cp) return 400;                                  // a line without ':' (Go: 400)
        const size_t colon = (size_t)((const char*)cp - d);
        // Name = token: an empty name, whitespace in the name ("X Y"), whitespace before ':' ("Content-Length : 5" —
        // smuggling when a strict front-end rejects and LOOK accepts) are all 400.
        if (colon == ls || !http_span_is(d + ls, colon - ls, 1)) return 400;
        size_t vs = colon + 1, ve = le;
        while (vs < ve && (d[vs] == ' ' || d[vs] == '\t')) ++vs;
        while (ve > vs && (d[ve - 1] == ' ' || d[ve - 1] == '\t')) --ve;
        if (!http_span_is(d + vs, ve - vs, 2)) return 400;   // NUL, CR, LF, DEL, other CTL
        std::string key(d + ls, colon - ls);
        for (auto& ch : key) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        std::string val(d + vs, ve - vs);
        // RFC 9112 §6.3: a double/inconsistent Content-Length leads to request smuggling.
        if (key == "content-length") {
            auto ex = req.headers.find("content-length");
            if (ex != req.headers.end() && ex->second != val) return 400;
        }
        // RFC 9112 §3.2: Host exactly once (Go: "too many Host headers" → 400). If the proxy and LOOK pick different ones of the two Hosts
        // it leads to virtual-host / cache poisoning.
        if (key == "host") {
            if (have_host) return 400;
            have_host = true;
            if (!http_host_ok(val)) return 400;
        }
        req.headers[std::move(key)] = std::move(val);
    }

    // Host is required in HTTP/1.1 (RFC 9112 §3.2, Go "missing required Host header").
    if (http11 && !have_host && req.method != "CONNECT") return 400;
    // in absolute-form the Host header is ignored, the authority part is used (RFC 9112 §3.2.2, Go r.Host).
    if (absolute_form && !abs_authority.empty()) req.headers["host"] = abs_authority;

    // WebSocket upgrade
    auto it_up  = req.headers.find("upgrade");
    auto it_key = req.headers.find("sec-websocket-key");
    if (it_up != req.headers.end()) {
        std::string up = it_up->second;
        std::transform(up.begin(), up.end(), up.begin(), ::tolower);
        if (up.find("websocket") != std::string::npos) {
            req.upgrade_websocket = true;
            if (it_key != req.headers.end()) req.ws_key = it_key->second;
        }
    }

    // SSE detection
    auto it_acc = req.headers.find("accept");
    if (it_acc != req.headers.end()) {
        if (it_acc->second.find("text/event-stream") != std::string::npos)
            req.upgrade_sse = true;
    }

    return 0;
}

} // namespace look

#pragma once
// multipart/form-data reader and media-type parser, pure functions (no I/O): the model is Go's mime/multipart,
// net/textproto and mime.ParseMediaType, and tests/multipart_differential_test.py checks it against Go on random bodies.
//
// Before, the reader looked for "\r\n--boundary" ANYWHERE and took whatever followed as the next part: a data line
// "--BoundaryX" ended the part early (an upload of 82 bytes read as 3), a preamble that mentions the boundary
// started the first part in the middle of a line, an "attachment" part or a repeated name/filename parameter was
// read as a field, a header folded onto a second line was ignored, and a body with LF line ends read as empty. A WAF or
// proxy that reads the body one way and the application another is how an upload gets smuggled. Go's rules:
//   * a delimiter is a line "--boundary" followed by spaces/tabs and a line end (the first one fixes the line end to
//     "\r\n" or "\n"); "--boundary--" is the end; lines before the first delimiter (the preamble) and after the end are
//     ignored; any other line between parts is an error;
//   * inside a part, "\r\n--boundary" ends the data only when it is followed by a space, a tab, CR, LF or "--", or by
//     the end of the body; a part with no delimiter after it is an error (the body was cut);
//   * the part's headers are read as net/textproto reads them: "Name: value" lines, a line starting with a space or
//     tab continues the one before, a line without a colon, a name that is not a token, or a control byte in a
//     value is an error;
//   * the field name and file name come from Content-Disposition read as mime.ParseMediaType does: the type must be
//     "form-data" (an "attachment" or "inline" part has no field name and is skipped), a repeated parameter or a
//     malformed one leaves the part without a name (skipped), quoted strings and RFC 2231 "filename*=UTF-8''..." and
//     "filename*0=" continuations are read;
//   * a part with Content-Transfer-Encoding: quoted-printable is refused (Go decodes it; LOOK says so with 400 instead).
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace look {

inline bool mime_token_char(unsigned char c) {
    return c > 0x20 && c < 0x7f && std::string("()<>@,;:\\\"/[]?=").find((char)c) == std::string::npos;
}

namespace mime_detail {
inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline std::string trim_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
inline int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// RFC 2231 "charset'language'percent-encoded" -> UTF-8; false when the charset is not one Go reads.
inline bool decode2231(const std::string& v, std::string& out) {
    size_t q1 = v.find('\''); if (q1 == std::string::npos) return false;
    size_t q2 = v.find('\'', q1 + 1); if (q2 == std::string::npos) return false;
    std::string cs = lower(v.substr(0, q1));
    if (cs != "us-ascii" && cs != "utf-8" && cs != "iso-8859-1") return false;
    std::string enc = v.substr(q2 + 1), raw;
    for (size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%') {
            if (i + 2 >= enc.size()) return false;
            int h = hexv(enc[i + 1]), l = hexv(enc[i + 2]);
            if (h < 0 || l < 0) return false;
            raw += (char)(h * 16 + l); i += 2;
        } else {
            unsigned char c = (unsigned char)enc[i];
            if (c <= 0x20 || c >= 0x7f) return false;       // not an attribute-char
            raw += enc[i];
        }
    }
    if (cs == "iso-8859-1") {
        std::string u;
        for (unsigned char c : raw) { if (c < 0x80) u += (char)c; else { u += (char)(0xC0 | (c >> 6)); u += (char)(0x80 | (c & 0x3F)); } }
        raw = u;
    } else if (cs == "us-ascii") {
        for (unsigned char c : raw) if (c >= 0x80) return false;
    }
    out = raw;
    return true;
}
inline std::string pct_piece(const std::string& v, bool& ok) {   // continuation piece: only percent-decoding
    std::string raw;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%') {
            if (i + 2 >= v.size()) { ok = false; return raw; }
            int h = hexv(v[i + 1]), l = hexv(v[i + 2]);
            if (h < 0 || l < 0) { ok = false; return raw; }
            raw += (char)(h * 16 + l); i += 2;
        } else raw += v[i];
    }
    return raw;
}
}  // namespace mime_detail

// Content-Type / Content-Disposition value -> media type (lower case) + parameters (names lower case). Go's
// mime.ParseMediaType: false = malformed (the caller treats the parameters as absent).
inline bool parse_media_type(const std::string& v, std::string& type, std::map<std::string, std::string>& params) {
    using namespace mime_detail;
    params.clear();
    size_t semi = v.find(';');
    type = lower(trim_ws(v.substr(0, semi)));
    {   // a token, or a token "/" token
        size_t slash = type.find('/');
        auto tok = [](const std::string& s) { if (s.empty()) return false; for (unsigned char c : s) if (!mime_token_char(c)) return false; return true; };
        if (slash == std::string::npos ? !tok(type) : !(tok(type.substr(0, slash)) && tok(type.substr(slash + 1)))) return false;
    }
    std::map<std::string, std::map<std::string, std::string>> cont;   // base name -> (name with '*' -> value)
    std::string rest = semi == std::string::npos ? "" : v.substr(semi);
    while (!rest.empty()) {
        size_t i = rest.find_first_not_of(" \t\r\n");
        if (i == std::string::npos) break;
        rest = rest.substr(i);
        if (rest[0] != ';') return false;
        rest = rest.substr(1);
        i = rest.find_first_not_of(" \t\r\n");
        if (i == std::string::npos) break;                                 // a trailing ';' is ignored
        rest = rest.substr(i);
        size_t k = 0;
        while (k < rest.size() && mime_token_char((unsigned char)rest[k])) ++k;
        if (k == 0) { if (trim_ws(rest) == "") break; return false; }
        std::string key = lower(rest.substr(0, k));
        rest = rest.substr(k);
        i = rest.find_first_not_of(" \t\r\n");
        if (i == std::string::npos || rest[i] != '=') return false;
        rest = rest.substr(i + 1);
        i = rest.find_first_not_of(" \t\r\n");
        rest = i == std::string::npos ? "" : rest.substr(i);
        std::string val;
        if (!rest.empty() && rest[0] == '"') {
            size_t j = 1; bool closed = false;
            while (j < rest.size()) {
                if (rest[j] == '\\' && j + 1 < rest.size()) { val.push_back(rest[j + 1]); j += 2; continue; }
                if (rest[j] == '"') { closed = true; ++j; break; }
                val.push_back(rest[j++]);
            }
            if (!closed) return false;
            rest = rest.substr(j);
        } else {
            size_t j = 0;
            while (j < rest.size() && mime_token_char((unsigned char)rest[j])) ++j;
            if (j == 0) return false;
            val = rest.substr(0, j);
            rest = rest.substr(j);
        }
        size_t star = key.find('*');
        if (star != std::string::npos) {
            auto& m = cont[key.substr(0, star)];
            if (m.count(key)) return false;                                 // duplicate parameter
            m[key] = val;
            continue;
        }
        auto dup = params.find(key);                                        // a repeated parameter: Go keeps it only if the value is the same
        if (dup != params.end() && dup->second != val) return false;
        params[key] = val;
    }
    for (auto& [base, pieces] : cont) {
        auto single = pieces.find(base + "*");
        if (single != pieces.end()) {
            std::string dec;
            if (decode2231(single->second, dec)) params[base] = dec;
            continue;
        }
        std::string all;
        for (int n = 0;; ++n) {
            std::string plain = base + "*" + std::to_string(n), ext = plain + "*";
            auto p = pieces.find(plain), e = pieces.find(ext);
            if (p != pieces.end()) all += p->second;
            else if (e != pieces.end()) {
                std::string piece;
                if (n == 0) { if (!decode2231(e->second, piece)) { all.clear(); break; } }
                else { bool ok = true; piece = pct_piece(e->second, ok); if (!ok) { all.clear(); break; } }
                all += piece;
            } else break;
        }
        if (!all.empty()) params[base] = all;
    }
    return true;
}

struct MultipartPart {
    std::string name;       // the form field name (Content-Disposition: form-data; name=...)
    std::string filename;   // empty = a field, not a file
    std::string data;
};

struct MultipartResult {
    std::string error;      // empty = the whole body was read; otherwise why not (the parts read before are dropped)
    bool        too_many = false;   // error is "more than max_parts parts"
    std::vector<MultipartPart> parts;   // parts with a field name only; others are skipped as Go's ReadForm skips them
};

inline MultipartResult parse_multipart_body(const std::string& body, const std::string& boundary, size_t max_parts) {
    using namespace mime_detail;
    MultipartResult R;
    const std::string dash = "--" + boundary;
    std::string nl = "\r\n";
    size_t pos = 0, parts_read = 0;
    auto skip_lws = [](const std::string& s, size_t from) { while (from < s.size() && (s[from] == ' ' || s[from] == '\t')) ++from; return from; };
    auto is_final = [&](const std::string& line) {
        if (line.compare(0, dash.size() + 2, dash + "--") != 0) return false;
        std::string rest = line.substr(dash.size() + 2);
        rest = rest.substr(skip_lws(rest, 0));
        return rest.empty() || rest == nl;
    };
    auto is_delim = [&](const std::string& line) {
        if (line.compare(0, dash.size(), dash) != 0) return false;
        std::string rest = line.substr(dash.size());
        rest = rest.substr(skip_lws(rest, 0));
        if (parts_read == 0 && rest == "\n") nl = "\n";                      // the first delimiter fixes the line end
        return rest == nl;
    };
    auto match_after = [&](size_t at, size_t plen) -> int {                  // +1 delimiter, -1 data, as Go's matchAfterPrefix
        if (at + plen >= body.size()) return +1;
        char c = body[at + plen];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return +1;
        if (c == '-') { if (at + plen + 1 >= body.size()) return -1; if (body[at + plen + 1] == '-') return +1; }
        return -1;
    };
    while (true) {
        // ── NextPart: find a delimiter line ──
        bool expect_new = false, got_part = false;
        while (true) {
            if (pos >= body.size()) { R.error = "multipart: NextPart: EOF"; return R; }
            size_t e = body.find('\n', pos);
            bool eof = (e == std::string::npos);
            // Go reads these lines through a 4096-byte buffer: a longer line (outside a part) is an error
            if ((eof ? body.size() - pos : e - pos + 1) > 4096) { R.error = "multipart: NextPart: bufio: buffer full"; return R; }
            std::string line = eof ? body.substr(pos) : body.substr(pos, e - pos + 1);
            pos = eof ? body.size() : e + 1;
            if (eof && is_final(line)) return R;
            if (eof) { R.error = "multipart: NextPart: EOF"; return R; }
            if (is_delim(line)) { ++parts_read; got_part = true; break; }
            if (is_final(line)) return R;                                    // the epilogue is ignored
            if (expect_new) { R.error = "multipart: NextPart: unexpected line"; return R; }
            if (parts_read == 0) continue;                                   // the preamble
            if (line == nl) { expect_new = true; continue; }
            R.error = "multipart: NextPart: unexpected line";
            return R;
        }
        (void)got_part;
        if (parts_read > max_parts) { R.too_many = true; R.error = "multipart: more than " + std::to_string(max_parts) + " parts"; return R; }
        // ── headers (net/textproto ReadMIMEHeader) ──
        std::vector<std::pair<std::string, std::string>> hdr;
        bool first = true;
        while (true) {
            if (pos >= body.size()) { R.error = "multipart: malformed MIME header: EOF"; return R; }
            size_t e = body.find('\n', pos);
            if (e == std::string::npos) { R.error = "multipart: malformed MIME header: EOF"; return R; }
            std::string line = body.substr(pos, e - pos);
            pos = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;                                         // the blank line ends the headers
            if (line[0] == ' ' || line[0] == '\t') {
                if (first || hdr.empty()) { R.error = "multipart: malformed MIME header initial line"; return R; }
                std::string cont = trim_ws(line);
                for (unsigned char c : cont) if ((c < 0x20 && c != 0x09) || c == 0x7f) { R.error = "multipart: malformed MIME header line"; return R; }
                if (!cont.empty()) hdr.back().second += (hdr.back().second.empty() ? "" : " ") + cont;
                continue;
            }
            first = false;
            size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) { R.error = "multipart: malformed MIME header line"; return R; }
            std::string key = line.substr(0, colon);
            for (unsigned char c : key) if (!mime_token_char(c)) { R.error = "multipart: malformed MIME header line"; return R; }
            std::string val = trim_ws(line.substr(colon + 1));
            for (unsigned char c : val) if ((c < 0x20 && c != 0x09) || c == 0x7f) { R.error = "multipart: malformed MIME header line"; return R; }
            hdr.emplace_back(lower(key), val);
        }
        // ── data: up to "\r\n--boundary" that is followed as a delimiter ──
        const std::string nldash = nl + dash;
        size_t end = std::string::npos;
        if (body.compare(pos, dash.size(), dash) == 0) {                     // an empty part: the delimiter follows at once
            int m = match_after(pos, dash.size());
            if (m > 0) end = pos;
        }
        if (end == std::string::npos) {
            size_t from = pos;
            while (true) {
                size_t j = body.find(nldash, from);
                if (j == std::string::npos) { R.error = "multipart: NextPart: unexpected EOF"; return R; }
                if (match_after(j, nldash.size()) > 0) { end = j; break; }
                from = j + nldash.size();
            }
        }
        std::string data = body.substr(pos, end - pos);
        pos = end;
        // ── what the part is ──
        std::string cd, cte;
        for (auto& h : hdr) {
            if (h.first == "content-disposition" && cd.empty() && !h.second.empty()) cd = h.second;
            if (h.first == "content-transfer-encoding" && cte.empty()) cte = h.second;
        }
        if (cte == "quoted-printable") { R.error = "multipart: Content-Transfer-Encoding quoted-printable is not supported"; return R; }
        MultipartPart part;
        std::string type; std::map<std::string, std::string> params;
        if (!cd.empty() && parse_media_type(cd, type, params) && type == "form-data") {
            auto n = params.find("name");
            if (n != params.end()) part.name = n->second;
            auto f = params.find("filename");
            if (f != params.end()) part.filename = f->second;
        }
        if (part.name.empty()) continue;                                     // skipped, as Go's ReadForm skips it
        part.data = std::move(data);
        R.parts.push_back(std::move(part));
    }
}

}  // namespace look

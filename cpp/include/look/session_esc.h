// session_esc.h — Session SID validation + blob escape/injection defence (PURE, table-test).
// web_stdlib.cpp session:: calls these. Two security decisions at a single point here:
//   1) valid_sid: 32-hex allowlist — rejects path traversal (sid → file path).
//   2) sess_esc/unesc + blob_get/set: blocks \n / = injection into the "key=value\n" format
//      (cuts off injection of a forged privilege field if user data is written to the session).
#pragma once
#include <string>

namespace look {

// SID = the generated format is EXACTLY 32 hex. It comes from the client's cookie and enters the file path.
inline bool valid_sid(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// A \n in a value can be LEGITIMATE user data (textarea) → do not reject, ESCAPE it.
// '=' in a key is also escaped (so that it does not act as a delimiter). Backward compatible: old
// blobs have no backslash → unescape acts as the identity function.
inline std::string sess_esc(const std::string& s, bool is_key) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        if      (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '='  && is_key) o += "\\e";
        else o += c;
    }
    return o;
}

inline std::string sess_unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        if      (n == 'n')  o += '\n';
        else if (n == 'r')  o += '\r';
        else if (n == 'e')  o += '=';
        else if (n == '\\') o += '\\';
        else { o += '\\'; o += n; }
    }
    return o;
}

inline bool sess_blob_get(const std::string& blob, const std::string& key, std::string& out) {
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == sess_esc(key, true)) {
            out = sess_unesc(line.substr(eq + 1));
            return true;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

// update key (replace if present, add if not) — also fixes the set-does-not-overwrite bug caused by the file backend's "append and
// get returns the first match".
inline void sess_blob_set(std::string& blob, const std::string& key, const std::string& val) {
    const std::string ek = sess_esc(key, true);    // cannot act as a delimiter
    const std::string ev = sess_esc(val, false);   // a \n injection would be here
    std::string out; size_t pos = 0; bool replaced = false;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
        if (!line.empty()) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) == ek) { out += ek + "=" + ev + "\n"; replaced = true; }
            else out += line + "\n";
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!replaced) out += ek + "=" + ev + "\n";
    blob = out;
}

} // namespace look

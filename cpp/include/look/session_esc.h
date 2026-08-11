// session_esc.h — Session SID doğrulama + blob kaçış/enjeksiyon savunması (SAF, tablo-test).
// web_stdlib.cpp session:: bunları çağırır. İki güvenlik-kararı burada tek noktada:
//   1) valid_sid: 32-hex allowlist — path-traversal (sid → dosya yolu) reddi.
//   2) sess_esc/unesc + blob_get/set: "anahtar=deger\n" formatına \n / = enjeksiyonu engeli
//      (kullanıcı verisi oturuma yazılırsa uydurma yetki-alanı enjeksiyonunu keser).
#pragma once
#include <string>

namespace look {

// SID = üretilen format TAM 32 hex. Client cookie'sinden gelir, dosya yoluna girer.
inline bool valid_sid(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// Değerdeki \n MEŞRU kullanıcı verisi olabilir (textarea) → reddetme, KAÇIR.
// Anahtarda '=' de kaçırılır (ayraç anlamı taşımasın). Geriye uyumlu: eski
// blob'larda ters-bölü yok → unescape kimlik işlevi görür.
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

// key'i güncelle (varsa değiştir, yoksa ekle) — dosya backend'inin "append edip
// get ilk eşleşmeyi döndürme" nedeniyle set'in overwrite etmeme bug'ını da giderir.
inline void sess_blob_set(std::string& blob, const std::string& key, const std::string& val) {
    const std::string ek = sess_esc(key, true);    // ayraç anlamı taşıyamaz
    const std::string ev = sess_esc(val, false);   // \n enjeksiyonu burada olurdu
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

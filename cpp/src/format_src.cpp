// lk fmt — kaynak biçimlendirici. Bkz. look/format_src.h (tasarım + güvence).
#include "look/format_src.h"
#include "look/lexer.h"
#include "look/token.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>

namespace look {

namespace {

// Bir satırı, taşınan string-durumuyla tara. in_str: 0=string dışı, aksi halde açık tırnak.
// Döner: net derinlik değişimi (delta), satır-başı kapatıcı sayısı (leading_closers).
// in_str referansla güncellenir (çok-satırlı string desteği).
struct ScanResult { int delta = 0; int leading_closers = 0; };

ScanResult scan_line(const std::string& s, char& in_str) {
    ScanResult r;
    bool leading = true;            // hâlâ satır-başı kapatıcılarını sayıyoruz
    bool line_comment = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (line_comment) break;
        if (in_str) {
            if (c == '\\') { if (i + 1 < s.size()) ++i; continue; }  // kaçış
            if (c == in_str) in_str = 0;                              // string kapandı
            continue;
        }
        // string dışı
        if (c == '#') { line_comment = true; break; }
        if (c == '/' && i + 1 < s.size() && s[i+1] == '/') { line_comment = true; break; }
        if (c == '"' || c == '\'') { in_str = c; leading = false; continue; }
        if (c == '{' || c == '[' || c == '(') { r.delta++; leading = false; continue; }
        if (c == '}' || c == ']' || c == ')') {
            r.delta--;
            if (leading) r.leading_closers++;
            continue;
        }
        if (c != ' ' && c != '\t') leading = false;
    }
    return r;
}

std::string rstrip(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r");
    return e == std::string::npos ? "" : s.substr(0, e + 1);
}
std::string lstrip(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    return b == std::string::npos ? "" : s.substr(b);
}

// Trivia-dışı token dizileri eşit mi (anlam-koruma doğrulaması). Lexer yorumları zaten
// atar; whitespace de token değil → yalnız gerçek program token'ları karşılaştırılır.
bool same_tokens(const std::string& a, const std::string& b) {
    try {
        auto ta = Lexer(a).scan_tokens();
        auto tb = Lexer(b).scan_tokens();
        if (ta.size() != tb.size()) return false;
        for (size_t i = 0; i < ta.size(); ++i) {
            if (ta[i].type != tb[i].type) return false;
            if (ta[i].lexeme != tb[i].lexeme) return false;
        }
        return true;
    } catch (...) {
        return false;   // biri parse edilemiyorsa güvenli tarafta kal
    }
}

} // namespace

std::string look_format_source(const std::string& src, bool* ok) {
    std::vector<std::string> lines;
    { std::stringstream ss(src); std::string ln; while (std::getline(ss, ln)) lines.push_back(ln); }

    std::string out;
    int depth = 0;
    char in_str = 0;
    int blank_run = 0;

    for (const std::string& raw : lines) {
        if (in_str) {
            // Çok-satırlı string İÇİ — satırı AYNEN koru (girinti string verisidir).
            out += raw; out += "\n";
            scan_line(raw, in_str);   // yalnız in_str güncellensin
            blank_run = 0;
            continue;
        }
        std::string content = rstrip(lstrip(raw));
        if (content.empty()) {
            if (++blank_run <= 1) out += "\n";   // ardışık boşları tek boşluğa indir
            continue;
        }
        blank_run = 0;

        char probe = in_str;                       // in_str burada 0
        ScanResult sr = scan_line(content, probe);
        int indent = depth - sr.leading_closers;
        if (indent < 0) indent = 0;
        out.append((size_t)indent * 4, ' ');
        out += content;
        out += "\n";

        depth += sr.delta;
        if (depth < 0) depth = 0;
        in_str = probe;                            // çok-satırlı string taşınır
    }

    // Sondaki tek newline garantisi (dosya sonu temizliği)
    while (out.size() >= 2 && out[out.size()-1] == '\n' && out[out.size()-2] == '\n') out.pop_back();
    if (out.empty() || out.back() != '\n') out += "\n";

    // ── ANLAM-KORUMA GÜVENCESİ ──────────────────────────────────────────────────
    bool safe = same_tokens(src, out);
    if (ok) *ok = safe;
    return safe ? out : src;   // güvenli değilse ORİJİNALİ döndür (bozma yok)
}

int run_fmt(const std::vector<std::string>& files, bool check) {
    auto read_file = [](const std::string& p, std::string& out) -> bool {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
    };

    if (files.empty()) {   // stdin → stdout
        std::stringstream ss; ss << std::cin.rdbuf();
        bool ok = false;
        std::string res = look_format_source(ss.str(), &ok);
        std::cout << res;
        if (!ok) { std::cerr << "lk fmt: uyarı — biçimleme atlandı (anlam-koruma başarısız); girdi aynen döndü\n"; return 2; }
        return 0;
    }

    int rc = 0;
    for (const std::string& file : files) {
        std::string src;
        if (!read_file(file, src)) { std::cerr << "lk fmt: okunamadı: " << file << "\n"; rc = 2; continue; }
        bool ok = false;
        std::string res = look_format_source(src, &ok);
        if (!ok) { std::cerr << "lk fmt: " << file << " — biçimleme atlandı (anlam-koruma başarısız, dosya değişmedi)\n"; rc = 2; continue; }
        if (res == src) {
            // zaten biçimli
        } else if (check) {
            std::cout << "biçimsiz: " << file << "\n";
            rc = 1;   // CI modu: biçimsiz dosya var
        } else {
            std::ofstream of(file, std::ios::binary | std::ios::trunc);
            of << res;
            std::cout << "biçimlendi: " << file << "\n";
        }
    }
    return rc;
}

} // namespace look

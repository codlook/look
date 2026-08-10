#pragma once
// IMAP saf ayrıştırıcılar — "dikişi aç" (pg_parse.h / smtp_parse.h deseni): ağdan-beslenen,
// PRE-AUTH parse mantığını I/O'dan ayır → tek inline tanım (drift yok), fuzz + tablo testi.
// imap_server.cpp bu header'ı include eder; ayrı .static tanım YOK.
#include <string>
#include <cctype>

namespace look {

// "tag SP command SP args" → tag, cmd (upper), args (kalan). Boşluksuz satır → hepsi tag.
inline void imap_parse_command(const std::string& line, std::string& tag,
                               std::string& cmd, std::string& args) {
    tag.clear(); cmd.clear(); args.clear();
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) { tag = line; return; }
    tag = line.substr(0, sp1);
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) { cmd = line.substr(sp1 + 1); }
    else { cmd = line.substr(sp1 + 1, sp2 - sp1 - 1); args = line.substr(sp2 + 1); }
    for (char& ch : cmd) ch = (char)std::toupper((unsigned char)ch);  // komut case-insensitive
}

// İki argüman çıkar (LOGIN "user" "pass" veya LOGIN user pass) — tırnaklıysa tırnak-sınırına.
inline void imap_split_two(const std::string& args, std::string& a, std::string& b) {
    a.clear(); b.clear();
    auto unquote = [](std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
        return s;
    };
    size_t i = 0;
    auto next_token = [&](std::string& tok) {
        while (i < args.size() && args[i] == ' ') i++;
        if (i >= args.size()) return false;
        if (args[i] == '"') {
            size_t e = args.find('"', i + 1);
            if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
            else { tok = args.substr(i, e - i + 1); i = e + 1; }
        } else {
            size_t e = args.find(' ', i);
            if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
            else { tok = args.substr(i, e - i); i = e; }
        }
        return true;
    };
    std::string t1, t2;
    if (next_token(t1)) a = unquote(t1);
    if (next_token(t2)) b = unquote(t2);
}

// GÜVENLİK: mailbox adının SAF (I/O'suz) doğrulaması — path traversal + enjeksiyon guard'ı.
// resolve_mailbox'ın fs-canonical containment'inden ÖNCE gelen string kapısı. `cleaned`
// tırnaksız adı döner; INBOX özel (çağıran kökü döndürür). Dönen false = reddet.
//   Belgeli geçmiş bug: `INBOX; rm -rf /` KABUL ediliyordu — kontroller yalnız \0/mutlak/'..'
//   bakıyordu, İÇERİDEKİ ayırıcı+`;`+boşluk geçiyordu. Mailbox adı bir YOL BİLEŞENİ: ayırıcı
//   ve kontrol karakteri içeremez. (fs-containment .cpp'de kalır — gerçek yol gerektirir.)
// Dönen: true + cleaned="INBOX" özel-durumda; true + temiz ad; false = geçersiz/traversal.
inline bool imap_mailbox_name_ok(const std::string& name, std::string& cleaned) {
    cleaned.clear();
    std::string mb = name;
    if (mb.size() >= 2 && mb.front() == '"' && mb.back() == '"') mb = mb.substr(1, mb.size() - 2);
    if (mb.empty() || mb == "INBOX") { cleaned = "INBOX"; return true; }   // INBOX = kök Maildir
    if (mb.find('\0') != std::string::npos) return false;
    if (mb.front() == '/' || mb.front() == '\\') return false;
    if (mb.find("..") != std::string::npos) return false;
    for (unsigned char c : mb)
        if (c == '/' || c == '\\' || c == ';' || c < 0x20 || c == 0x7F) return false;
    cleaned = mb;
    return true;
}

} // namespace look

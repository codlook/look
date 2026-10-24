#pragma once
// PostgreSQL INSERT'in kimliği (db::last_id) — saf SQL yardımcıları (sunucusuz tablo testi:
// tests/pg_insert_test.cpp).
//
// ESKİ HATA (sessiz veri kaybı + yanlış kimlik): her INSERT'ten sonra `SELECT lastval()`
// soruluyordu. Dizisiz tabloda bu hata verir; transaction içinde hata transaction'ı iptal eder
// ve ardından gelen COMMIT sessizce ROLLBACK olur — satırlar hatasız kayboluyordu. lastval()
// ayrıca OTURUMUN son dizi değeridir, tablonun değil: dizisiz tabloya INSERT'ten sonra önceki
// tablonun (havuzdan gelen bağlantıda başka bir isteğin) kimliğini veriyordu.
// Şimdi: hedef tablonun dizi/IDENTITY kolonu katalogdan sorulur (hata veremez), INSERT'e
// `RETURNING "<kolon>"` eklenir, kimlik eklenen satırdan okunur.
#include <cctype>
#include <string>

namespace look {

// İlk sözcüğü INSERT mi (baştaki boşluk atlanır).
inline bool pg_is_insert(const std::string& sql) {
    size_t i = 0;
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    static const char* kw = "INSERT";
    for (int k = 0; k < 6; ++k, ++i)
        if (i >= sql.size() || std::toupper((unsigned char)sql[i]) != kw[k]) return false;
    return i >= sql.size() || !(std::isalnum((unsigned char)sql[i]) || sql[i] == '_');
}

namespace pg_insert_detail {
inline bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
inline bool ident_char(char c)  { return std::isalnum((unsigned char)c) || c == '_' || c == '$' || (unsigned char)c >= 0x80; }
inline void skip_ws(const std::string& s, size_t& i) { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
inline bool keyword(const std::string& s, size_t& i, const char* kw) {
    size_t j = i;
    for (; *kw; ++kw, ++j) if (j >= s.size() || std::toupper((unsigned char)s[j]) != *kw) return false;
    if (j < s.size() && ident_char(s[j])) return false;
    i = j; return true;
}
} // namespace pg_insert_detail

// "INSERT INTO <tablo>" — tablo adı SQL'de yazıldığı gibi ([şema.]ad; her parça tırnaksız
// tanımlayıcı ya da "tırnaklı"). Sözdizimi tanınmazsa "" (o zaman kimlik aranmaz, last_id 0).
// Ad katalog sorgusuna yalnız bu biçimdeyse gider → to_regclass sözdizimi hatası veremez.
inline std::string pg_insert_target(const std::string& sql) {
    using namespace pg_insert_detail;
    size_t i = 0; skip_ws(sql, i);
    if (!keyword(sql, i, "INSERT")) return "";
    skip_ws(sql, i);
    if (!keyword(sql, i, "INTO")) return "";
    skip_ws(sql, i);
    const size_t start = i;
    for (int part = 0; part < 3; ++part) {
        if (i < sql.size() && sql[i] == '"') {
            ++i; bool closed = false;
            while (i < sql.size()) {
                if (sql[i] == '"') { if (i + 1 < sql.size() && sql[i + 1] == '"') { i += 2; continue; } ++i; closed = true; break; }
                ++i;
            }
            if (!closed || i - start < 3) return "";
        } else if (i < sql.size() && ident_start(sql[i])) {
            while (i < sql.size() && ident_char(sql[i])) ++i;
        } else return "";
        if (i < sql.size() && sql[i] == '.') { ++i; continue; }
        break;
    }
    if (i < sql.size() && !(std::isspace((unsigned char)sql[i]) || sql[i] == '(')) return "";
    return sql.substr(start, i - start);
}

// Kullanıcının SQL'inde (tırnak ve yorum dışında) RETURNING sözcüğü var mı.
inline bool pg_has_returning(const std::string& sql) {
    using namespace pg_insert_detail;
    for (size_t i = 0; i < sql.size(); ) {
        char c = sql[i];
        if (c == '\'' || c == '"') {
            ++i;
            while (i < sql.size()) { if (sql[i] == c) { if (i + 1 < sql.size() && sql[i + 1] == c) { i += 2; continue; } ++i; break; } ++i; }
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') { while (i < sql.size() && sql[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            size_t e = sql.find("*/", i + 2); i = (e == std::string::npos) ? sql.size() : e + 2; continue;
        }
        if (ident_start(c) && (i == 0 || !ident_char(sql[i - 1]))) {
            size_t j = i;
            if (keyword(sql, j, "RETURNING")) return true;
            while (i < sql.size() && ident_char(sql[i])) ++i;
            continue;
        }
        ++i;
    }
    return false;
}

// SQL'in sonuna RETURNING "<kolon>" (sondaki boşluk ve ; atılır; kolon adı tırnaklanır).
inline std::string pg_with_returning(const std::string& sql, const std::string& col) {
    size_t end = sql.size();
    while (end > 0 && (std::isspace((unsigned char)sql[end - 1]) || sql[end - 1] == ';')) --end;
    std::string q = "\"";
    for (char c : col) { if (c == '"') q += '"'; q += c; }
    q += '"';
    // Yeni satırda: SQL bir -- yorumuyla bitiyorsa RETURNING yorumun içinde kalmasın.
    return sql.substr(0, end) + "\nRETURNING " + q;
}

} // namespace look

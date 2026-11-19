#pragma once
// The identity of a PostgreSQL INSERT (db::last_id) — pure SQL helpers (server-less table test:
// tests/pg_insert_test.cpp).
//
// OLD BUG (silent data loss + wrong id): `SELECT lastval()` was asked after every
// INSERT. On a table without a sequence this errors; inside a transaction the error aborts the transaction
// and the COMMIT that follows silently becomes a ROLLBACK — rows vanished without an error. lastval()
// is also the SESSION's last sequence value, not the table's: after an INSERT into a table without a sequence it gave
// the id of the previous table (on a pooled connection, of another request).
// Now: the target table's sequence/IDENTITY column is asked from the catalog (cannot error), `RETURNING
// "<column>"` is added to the INSERT, and the id is read from the inserted row.
#include <cctype>
#include <string>

namespace look {

// Is the first word INSERT (leading whitespace is skipped).
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

// "INSERT INTO <table>" — the table name as written in the SQL ([schema.]name; each part an unquoted
// identifier or "quoted"). If the syntax is not recognized "" (then no id is looked up, last_id 0).
// The name goes to the catalog query only in this form → to_regclass cannot raise a syntax error.
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

// Does the user's SQL contain the word RETURNING (outside quotes and comments).
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

// Add RETURNING "<column>" to the end of the SQL (trailing whitespace and ; dropped; the column name is quoted).
inline std::string pg_with_returning(const std::string& sql, const std::string& col) {
    size_t end = sql.size();
    while (end > 0 && (std::isspace((unsigned char)sql[end - 1]) || sql[end - 1] == ';')) --end;
    std::string q = "\"";
    for (char c : col) { if (c == '"') q += '"'; q += c; }
    q += '"';
    // On a new line: if the SQL ends with a -- comment, RETURNING must not stay inside the comment.
    return sql.substr(0, end) + "\nRETURNING " + q;
}

} // namespace look

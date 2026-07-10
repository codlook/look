#include "look/sqlite_client.h"
#include "sqlite3/sqlite-amalgamation-3470200/sqlite3.h"
#include <stdexcept>

namespace look {

SqliteClient::SqliteClient() = default;

SqliteClient::~SqliteClient() {
    close();
}

void SqliteClient::open(const std::string& path) {
    if (db_) close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("sqlite: cannot open database '" + path + "': " + err);
    }
    // WAL modu — okuma/yazma çakışmasını azaltır
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // Foreign key kontrolünü etkinleştir
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

void SqliteClient::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::vector<DbRow> SqliteClient::query(const std::string& sql) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_) + " — SQL: " + sql);
    }

    std::vector<DbRow> rows;
    int col_count = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbRow row;
        for (int i = 0; i < col_count; i++) {
            std::string col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);

            DbValue dv;
            switch (col_type) {
                case SQLITE_INTEGER:
                    dv.str  = std::to_string(sqlite3_column_int64(stmt, i));
                    dv.type = sqlite_type::INTEGER;
                    break;
                case SQLITE_FLOAT:
                    dv.str  = std::to_string(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT;
                    break;
                case SQLITE_NULL:
                    dv.str     = "";
                    dv.type    = sqlite_type::NUL;
                    dv.is_null = true;
                    break;
                case SQLITE_BLOB:
                    // BLOB'u hex string olarak döndür
                    {
                        const uint8_t* data = (const uint8_t*)sqlite3_column_blob(stmt, i);
                        int bytes = sqlite3_column_bytes(stmt, i);
                        std::string hex;
                        hex.reserve(bytes * 2);
                        static const char digits[] = "0123456789abcdef";
                        for (int b = 0; b < bytes; b++) {
                            hex += digits[data[b] >> 4];
                            hex += digits[data[b] & 0xF];
                        }
                        dv.str  = hex;
                        dv.type = sqlite_type::BLOB;
                    }
                    break;
                default: // SQLITE_TEXT
                    {
                        const char* txt = (const char*)sqlite3_column_text(stmt, i);
                        dv.str  = txt ? txt : "";
                        dv.type = sqlite_type::TEXT;
                    }
                    break;
            }
            row.push_back({col_name, dv});
        }
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("sqlite: step error: ") + sqlite3_errmsg(db_));
    }

    // INSERT/UPDATE/DELETE için istatistikleri güncelle
    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }

    return rows;
}

std::vector<DbRow> SqliteClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_));

    // Parametre binding — gerçek prepared statement, string escape yok
    for (int i = 0; i < (int)params.size(); i++) {
        const auto& p = params[i];
        switch (p.kind) {
            case DbParam::NULL_VAL:  sqlite3_bind_null(stmt, i+1); break;
            case DbParam::INT_VAL:   sqlite3_bind_int64(stmt, i+1, p.i); break;
            case DbParam::FLOAT_VAL: sqlite3_bind_double(stmt, i+1, p.d); break;
            case DbParam::BOOL_VAL:  sqlite3_bind_int(stmt, i+1, p.b ? 1 : 0); break;
            case DbParam::TEXT_VAL:  sqlite3_bind_text(stmt, i+1, p.s.c_str(), (int)p.s.size(), SQLITE_TRANSIENT); break;
        }
    }

    std::vector<DbRow> rows;
    int col_count = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbRow row;
        for (int i = 0; i < col_count; i++) {
            std::string col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);
            DbValue dv;
            switch (col_type) {
                case SQLITE_INTEGER:
                    dv.str  = std::to_string(sqlite3_column_int64(stmt, i));
                    dv.type = sqlite_type::INTEGER; break;
                case SQLITE_FLOAT:
                    dv.str  = std::to_string(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT; break;
                case SQLITE_NULL:
                    dv.is_null = true; dv.type = sqlite_type::NUL; break;
                default: {
                    const char* txt = (const char*)sqlite3_column_text(stmt, i);
                    dv.str  = txt ? txt : "";
                    dv.type = sqlite_type::TEXT; break;
                }
            }
            row.push_back({col_name, dv});
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }
    return rows;
}

} // namespace look

#pragma once

#include "look/db_connection.h"
#include <string>
#include <stdexcept>

// Forward declare — sqlite3.h'ı include etmiyoruz, header kirliliği önlemek için
struct sqlite3;

namespace look {

class SqliteClient : public DbConnection {
public:
    SqliteClient();
    ~SqliteClient() override;

    // path: dosya yolu veya ":memory:"
    void open(const std::string& path);

    // DbConnection arayüzü
    std::vector<DbRow> query(const std::string& sql) override;
    std::vector<DbRow> execute(const std::string& sql, const std::vector<DbParam>& params) override;
    int64_t last_insert_id() const override { return last_insert_id_; }
    int64_t affected_rows()  const override { return affected_rows_;  }
    void    close()          override;
    bool    is_connected()   const override { return db_ != nullptr; }
    const char* driver_name() const override { return "sqlite"; }

private:
    sqlite3* db_             = nullptr;
    int64_t  last_insert_id_ = 0;
    int64_t  affected_rows_  = 0;
};

} // namespace look

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace look {

// A single cell value
struct DbValue {
    std::string str;
    uint8_t     type    = 0xFE;  // driver-specific type code
    bool        is_null = false; // true if SQL NULL
};

using DbRow = std::vector<std::pair<std::string, DbValue>>;

// SQLite type constants -- range 0x81-0x85 (does not collide with MySQL 0x01-0x1F)
namespace sqlite_type {
    static constexpr uint8_t INTEGER = 0x81;
    static constexpr uint8_t FLOAT   = 0x82;
    static constexpr uint8_t TEXT    = 0x83;
    static constexpr uint8_t BLOB    = 0x84;
    static constexpr uint8_t NUL     = 0x85;
}

// Driver-independent parameter type -- real prepared statement binding without depending on Value
struct DbParam {
    enum Kind { NULL_VAL, INT_VAL, FLOAT_VAL, TEXT_VAL, BOOL_VAL } kind = NULL_VAL;
    int64_t     i = 0;
    double      d = 0.0;
    std::string s;
    bool        b = false;

    static DbParam null()                       { DbParam p; p.kind = NULL_VAL;  return p; }
    static DbParam from_int(int64_t v)          { DbParam p; p.kind = INT_VAL;   p.i = v; return p; }
    static DbParam from_float(double v)         { DbParam p; p.kind = FLOAT_VAL; p.d = v; return p; }
    static DbParam from_text(std::string v)     { DbParam p; p.kind = TEXT_VAL;  p.s = std::move(v); return p; }
    static DbParam from_bool(bool v)            { DbParam p; p.kind = BOOL_VAL;  p.b = v; return p; }
};

// Abstract DB connection interface.
class DbConnection {
public:
    int tx_depth = 0;  // nested transaction depth (0 = autocommit)

    virtual ~DbConnection() = default;

    // For parameterless DDL / internal queries (driver internal)
    virtual std::vector<DbRow> query(const std::string& sql) = 0;

    // Parameterized query with a real prepared statement -- safe against SQL injection
    // ? placeholders are replaced by driver-native binding
    virtual std::vector<DbRow> execute(const std::string& sql,
                                       const std::vector<DbParam>& params) = 0;

    virtual int64_t last_insert_id() const = 0;
    virtual int64_t affected_rows()  const = 0;
    virtual void    close()                = 0;
    virtual bool    is_connected()   const = 0;
    virtual const char* driver_name() const = 0;

    // Transaction start statement -- driver-specific. In SQLite WAL mode a plain "BEGIN"
    // (=DEFERRED) produces SQLITE_BUSY_SNAPSHOT on the first write and busy_timeout CANNOT
    // retry it (it would break snapshot isolation). SQLite takes the write lock up front with
    // "BEGIN IMMEDIATE". In MySQL/PG "BEGIN IMMEDIATE" is invalid -> default "BEGIN".
    virtual const char* begin_stmt() const { return "BEGIN"; }
    // Is there an open transaction according to the server (MySQL SERVER_STATUS_IN_TRANS, PG ReadyForQuery state,
    // SQLite autocommit off). Independent of what the application called: db::exec("BEGIN") counts too.
    // An open transaction is rolled back when the connection returns to the pool (see release_thread_connections).
    virtual bool in_transaction() const { return tx_depth > 0; }
};

} // namespace look

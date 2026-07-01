#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace look {

// Tek bir hücre değeri
struct DbValue {
    std::string str;
    uint8_t     type    = 0xFE;  // driver'a özgü tip kodu
    bool        is_null = false; // SQL NULL ise true
};

using DbRow = std::vector<std::pair<std::string, DbValue>>;

// SQLite type sabitleri — 0x81-0x85 aralığı (MySQL 0x01-0x1F ile çakışmaz)
namespace sqlite_type {
    static constexpr uint8_t INTEGER = 0x81;
    static constexpr uint8_t FLOAT   = 0x82;
    static constexpr uint8_t TEXT    = 0x83;
    static constexpr uint8_t BLOB    = 0x84;
    static constexpr uint8_t NUL     = 0x85;
}

// Driver-bağımsız parametre tipi — Value'ya bağımlılık olmadan real prepared statement binding
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

// Soyut DB bağlantı arayüzü.
class DbConnection {
public:
    virtual ~DbConnection() = default;

    // Parametresiz DDL / dahili sorgular için (driver internal)
    virtual std::vector<DbRow> query(const std::string& sql) = 0;

    // Gerçek prepared statement ile parameterize sorgu — SQL injection güvenli
    // ? placeholder'ları driver-native binding ile değiştirilir
    virtual std::vector<DbRow> execute(const std::string& sql,
                                       const std::vector<DbParam>& params) = 0;

    virtual int64_t last_insert_id() const = 0;
    virtual int64_t affected_rows()  const = 0;
    virtual void    close()                = 0;
    virtual bool    is_connected()   const = 0;
    virtual const char* driver_name() const = 0;
};

} // namespace look

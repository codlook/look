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

// Soyut DB bağlantı arayüzü.
// Yalnızca tüm DB'lerin ortak yapabileceği işlemler buradadır.
// MySQL'e özgü ping(), reconnect() bu arayüzde YOK.
class DbConnection {
public:
    virtual ~DbConnection() = default;

    // SELECT → satır listesi (boş olabilir)
    // INSERT/UPDATE/DELETE → boş liste
    // sql: bind_params() ile hazırlanmış, ? yerine değerler geçirilmiş
    virtual std::vector<DbRow> query(const std::string& sql) = 0;

    // Son INSERT'teki otomatik artan ID
    virtual int64_t last_insert_id() const = 0;

    // Son DML'in etkilediği satır sayısı
    virtual int64_t affected_rows() const = 0;

    // Bağlantıyı kapat
    virtual void close() = 0;

    // Bağlantı açık mı?
    virtual bool is_connected() const = 0;

    // "mysql", "sqlite", "postgres" — log ve hata mesajları için
    virtual const char* driver_name() const = 0;
};

} // namespace look

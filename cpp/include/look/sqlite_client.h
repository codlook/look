#pragma once

#include "look/db_connection.h"
#include <string>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Forward declare — we do not include sqlite3.h, to avoid header pollution
struct sqlite3;
struct sqlite3_stmt;

namespace look {

// Serialize SQLite's once-only global init (sqlite3_initialize) AT PROCESS START, BEFORE
// threads. Otherwise when two workers call the first sqlite3_open at the same time there is a TSan DATA RACE
// on the isInit flag inside sqlite3_initialize (the t4 enforced
// guard caught this in CI). If called once at startup, the isInit write happens-before all worker
// reads → no race. Idempotent (a repeated call is harmless).
void sqlite_global_init();

class SqliteClient : public DbConnection {
public:
    SqliteClient();
    ~SqliteClient() override;

    // path: file path or ":memory:"
    void open(const std::string& path);

    // DbConnection interface
    std::vector<DbRow> query(const std::string& sql) override;
    std::vector<DbRow> execute(const std::string& sql, const std::vector<DbParam>& params) override;
    int64_t last_insert_id() const override { return last_insert_id_; }
    int64_t affected_rows()  const override { return affected_rows_;  }
    void    close()          override;
    bool    is_connected()   const override { return db_ != nullptr; }
    const char* driver_name() const override { return "sqlite"; }
    const char* begin_stmt()  const override { return "BEGIN IMMEDIATE"; }
    bool in_transaction() const override;   // sqlite3_get_autocommit == 0

private:
    sqlite3* db_             = nullptr;
    int64_t  last_insert_id_ = 0;
    int64_t  affected_rows_  = 0;

    // Connections writing to the same file in this process write IN TURN (first come, first served); see .cpp.
    std::shared_ptr<struct SqliteWriteGate> gate_;
    bool gate_held_ = false;
    void gate_before(::sqlite3_stmt* stmt, const std::string& sql);
    void gate_after();

    // A query waiting inside a fiber makes the fiber wait, not the thread (see .cpp).
    bool in_fiber_ = false;                               // the query is currently inside a fiber, on this thread
    std::chrono::steady_clock::time_point slice_start_;   // moment of the last give-up of the turn
    int  busy_waited_ms_ = 0;
    static int on_progress(void* self);
    static int on_busy(void* self, int count);
    std::unique_ptr<struct SqliteRunner> runner_;         // while the write gate is full: the connection's thread
    template <class Fn> std::vector<DbRow> run(Fn&& fn);
    template <class Fn> std::vector<DbRow> on_own_thread(Fn&& fn);
    std::vector<DbRow> query_here(const std::string& sql);
    std::vector<DbRow> execute_here(const std::string& sql, const std::vector<DbParam>& params);

    // Prepared-statement cache (per connection): the same SQL is not re-parsed on every query.
    // The statement in use is TAKEN OUT of the cache, and when finished it is reset and put back — two uses do not share the same
    // statement, and an unreset statement does not keep a read open. When the schema changes SQLite
    // re-prepares the statement itself (prepare_v2).
    struct CachedStmt { ::sqlite3_stmt* stmt; uint64_t used; };
    std::unordered_map<std::string, CachedStmt> stmt_cache_;
    uint64_t stmt_tick_ = 0;
    static constexpr size_t STMT_CACHE_MAX = 64;
    ::sqlite3_stmt* take_stmt(const std::string& sql);
    void give_back_stmt(const std::string& sql, ::sqlite3_stmt* st);
    void clear_stmt_cache();
};

} // namespace look

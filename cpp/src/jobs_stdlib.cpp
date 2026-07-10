// jobs_stdlib.cpp — jobs:: module, SQLite-backed persistent job queue
// Status lifecycle: pending → processing → done
//                                       ↘ failed  (retry_count >= max_retries)
//                             fail()  → pending   (if retry_count < max_retries)
// Delayed jobs: run_after = now + delay_seconds  (next() filters run_after <= now)
#include "sqlite3/sqlite-amalgamation-3470200/sqlite3.h"
#include "look/jobs_store.h"
#include "look/logger.h"
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace look {

// ── Singleton ──────────────────────────────────────────────────────────────
JobStore& JobStore::instance() {
    static JobStore inst;
    return inst;
}

JobStore::~JobStore() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

int64_t JobStore::now_ts() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// ── init ──────────────────────────────────────────────────────────────────
void JobStore::init(const std::string& db_path) {
    if (initialized_) return;

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "bilinmeyen hata";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("jobs:: DB açılamadı (" + db_path + "): " + err);
    }

    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=OFF;", nullptr, nullptr, nullptr);

    create_schema();
    initialized_ = true;
}

void JobStore::ensure_init() {
    if (initialized_) return;
    const char* env_path = std::getenv("JOBS_DB");
    std::string path = env_path ? env_path : "jobs.db";
    init(path);
}

void JobStore::create_schema() {
    // Main table
    const char* sql =
        "CREATE TABLE IF NOT EXISTS look_jobs ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  queue       TEXT    NOT NULL,"
        "  payload     TEXT    NOT NULL,"
        "  status      TEXT    NOT NULL DEFAULT 'pending',"
        "  retry_count INTEGER NOT NULL DEFAULT 0,"
        "  max_retries INTEGER NOT NULL DEFAULT 3,"
        "  run_after   INTEGER NOT NULL DEFAULT 0,"
        "  created_at  INTEGER NOT NULL,"
        "  updated_at  INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_jobs_qs ON look_jobs(queue, status, run_after, id);";

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "?";
        sqlite3_free(errmsg);
        throw std::runtime_error("jobs:: schema hatası: " + err);
    }

    // Schema migration: add run_after to existing DBs (ignore error if column exists)
    sqlite3_exec(db_,
        "ALTER TABLE look_jobs ADD COLUMN run_after INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);
}

// ── helpers ────────────────────────────────────────────────────────────────
static sqlite3_stmt* prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
        throw std::runtime_error(std::string("jobs:: SQL hazırlık hatası: ") + sqlite3_errmsg(db));
    return stmt;
}

static Value make_assoc(std::initializer_list<std::pair<std::string, Value>> kvs) {
    auto arr = std::make_shared<std::vector<Value>>();
    arr->push_back(Value(std::string("__assoc__")));  // single sentinel
    for (auto& [k, v] : kvs) {
        arr->push_back(Value(k));
        arr->push_back(v);
    }
    return Value(arr);
}

// Safely read keys from a LOOK assoc Value — format: ["__assoc__", key, val, ...]
static std::string assoc_str(const Value& assoc, const std::string& key) {
    auto ap = assoc.as_array();
    if (!ap || ap->empty()) return "";
    auto& arr = *ap;
    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
        if (arr[i].type() == Value::STRING && arr[i].as_string() == key)
            return arr[i + 1].to_string();
    }
    return "";
}

static int64_t assoc_int(const Value& assoc, const std::string& key) {
    auto ap = assoc.as_array();
    if (!ap || ap->empty()) return 0;
    auto& arr = *ap;
    for (size_t i = 1; i + 1 < arr.size(); i += 2) {
        if (arr[i].type() == Value::STRING && arr[i].as_string() == key)
            return (int64_t)arr[i + 1].to_float();
    }
    return 0;
}

// ── push ──────────────────────────────────────────────────────────────────
int64_t JobStore::push(const std::string& queue, const std::string& payload_json,
                       int max_retries, int delay_seconds) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    int64_t ts       = now_ts();
    int64_t run_after = ts + delay_seconds;

    sqlite3_stmt* stmt = prepare(db_,
        "INSERT INTO look_jobs(queue,payload,status,retry_count,max_retries,run_after,created_at,updated_at)"
        " VALUES(?,?,'pending',0,?,?,?,?)");
    sqlite3_bind_text (stmt, 1, queue.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, payload_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, 3, max_retries);
    sqlite3_bind_int64(stmt, 4, run_after);
    sqlite3_bind_int64(stmt, 5, ts);
    sqlite3_bind_int64(stmt, 6, ts);
    int rc = sqlite3_step(stmt);
    int64_t id = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(db_) : 0;
    sqlite3_finalize(stmt);
    return id;
}

// ── next ──────────────────────────────────────────────────────────────────
Value JobStore::next(const std::string& queue) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    // Only jobs whose run_after <= now (delayed jobs wait)
    sqlite3_stmt* sel = prepare(db_,
        "SELECT id,payload,retry_count,max_retries,run_after FROM look_jobs"
        " WHERE queue=? AND status='pending' AND run_after<=?"
        " ORDER BY id ASC LIMIT 1");
    int64_t ts = now_ts();
    sqlite3_bind_text (sel, 1, queue.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(sel, 2, ts);

    int rc = sqlite3_step(sel);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(sel);
        return Value();
    }
    int64_t     id          = sqlite3_column_int64(sel, 0);
    std::string payload     = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
    int         retry_count = sqlite3_column_int(sel, 2);
    int         max_retries = sqlite3_column_int(sel, 3);
    int64_t     run_after   = sqlite3_column_int64(sel, 4);
    sqlite3_finalize(sel);

    // Claim: pending → processing
    sqlite3_stmt* upd = prepare(db_,
        "UPDATE look_jobs SET status='processing', updated_at=? WHERE id=?");
    sqlite3_bind_int64(upd, 1, now_ts());
    sqlite3_bind_int64(upd, 2, id);
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    return make_assoc({
        {"id",          Value((int)id)},
        {"payload",     Value(payload)},
        {"queue",       Value(queue)},
        {"retry_count", Value(retry_count)},
        {"max_retries", Value(max_retries)},
        {"run_after",   Value((int)run_after)},
    });
}

// ── done ──────────────────────────────────────────────────────────────────
void JobStore::done(int64_t id) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* stmt = prepare(db_,
        "UPDATE look_jobs SET status='done', updated_at=? WHERE id=?");
    sqlite3_bind_int64(stmt, 1, now_ts());
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ── fail ──────────────────────────────────────────────────────────────────
void JobStore::fail(int64_t id) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* sel = prepare(db_,
        "SELECT retry_count, max_retries FROM look_jobs WHERE id=?");
    sqlite3_bind_int64(sel, 1, id);
    int retry_count = 0, max_retries = 3;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        retry_count = sqlite3_column_int(sel, 0);
        max_retries = sqlite3_column_int(sel, 1);
    }
    sqlite3_finalize(sel);

    int         new_count  = retry_count + 1;
    const char* new_status = (new_count >= max_retries) ? "failed" : "pending";

    sqlite3_stmt* upd = prepare(db_,
        "UPDATE look_jobs SET status=?, retry_count=?, updated_at=? WHERE id=?");
    sqlite3_bind_text (upd, 1, new_status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (upd, 2, new_count);
    sqlite3_bind_int64(upd, 3, now_ts());
    sqlite3_bind_int64(upd, 4, id);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
}

// ── stats ─────────────────────────────────────────────────────────────────
Value JobStore::stats(const std::string& queue) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* stmt = prepare(db_,
        "SELECT status, COUNT(*) FROM look_jobs WHERE queue=? GROUP BY status");
    sqlite3_bind_text(stmt, 1, queue.c_str(), -1, SQLITE_TRANSIENT);

    std::map<std::string, int> counts;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        counts[s] = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    return make_assoc({
        {"pending",    Value(counts.count("pending")    ? counts["pending"]    : 0)},
        {"processing", Value(counts.count("processing") ? counts["processing"] : 0)},
        {"done",       Value(counts.count("done")       ? counts["done"]       : 0)},
        {"failed",     Value(counts.count("failed")     ? counts["failed"]     : 0)},
    });
}

// ── list ──────────────────────────────────────────────────────────────────
Value JobStore::list(const std::string& queue, const std::string& status, int limit) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* stmt = prepare(db_,
        "SELECT id,payload,retry_count,max_retries,run_after,created_at,updated_at"
        " FROM look_jobs WHERE queue=? AND status=? ORDER BY id LIMIT ?");
    sqlite3_bind_text(stmt, 1, queue.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, limit);

    auto result = std::make_shared<std::vector<Value>>();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t     id  = sqlite3_column_int64(stmt, 0);
        std::string pay = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        int         rc  = sqlite3_column_int(stmt, 2);
        int         mr  = sqlite3_column_int(stmt, 3);
        int64_t     ra  = sqlite3_column_int64(stmt, 4);
        int64_t     cr  = sqlite3_column_int64(stmt, 5);
        int64_t     up  = sqlite3_column_int64(stmt, 6);

        result->push_back(make_assoc({
            {"id",          Value((int)id)},
            {"payload",     Value(pay)},
            {"status",      Value(status)},
            {"retry_count", Value(rc)},
            {"max_retries", Value(mr)},
            {"run_after",   Value((int)ra)},
            {"created_at",  Value((int)cr)},
            {"updated_at",  Value((int)up)},
        }));
    }
    sqlite3_finalize(stmt);
    return Value(result);
}

// ── retry ─────────────────────────────────────────────────────────────────
void JobStore::retry(int64_t id) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* stmt = prepare(db_,
        "UPDATE look_jobs SET status='pending', updated_at=? WHERE id=? AND status='failed'");
    sqlite3_bind_int64(stmt, 1, now_ts());
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ── purge ─────────────────────────────────────────────────────────────────
int64_t JobStore::purge(const std::string& queue, const std::string& status) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* stmt = prepare(db_,
        "DELETE FROM look_jobs WHERE queue=? AND status=?");
    sqlite3_bind_text(stmt, 1, queue.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int64_t deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

// ── recover ───────────────────────────────────────────────────────────────
// Crash recovery: processing → pending for jobs stuck longer than min_age_seconds.
// Call at startup before jobs::run() to rescue orphaned jobs from a previous crash.
int64_t JobStore::recover(const std::string& queue, int min_age_seconds) {
    ensure_init();
    std::lock_guard<std::mutex> lk(mtx_);

    const char* sql = (min_age_seconds <= 0)
        ? "UPDATE look_jobs SET status='pending', updated_at=?"
          " WHERE queue=? AND status='processing'"
        : "UPDATE look_jobs SET status='pending', updated_at=?"
          " WHERE queue=? AND status='processing'"
          " AND updated_at <= ?";

    sqlite3_stmt* stmt = prepare(db_, sql);
    int64_t ts = now_ts();
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text (stmt, 2, queue.c_str(), -1, SQLITE_TRANSIENT);
    if (min_age_seconds > 0)
        sqlite3_bind_int64(stmt, 3, ts - min_age_seconds);

    sqlite3_step(stmt);
    int64_t recovered = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return recovered;
}

// ── register_worker ───────────────────────────────────────────────────────
void JobStore::register_worker(const std::string& queue, Value fn) {
    workers_.push_back({queue, std::move(fn)});
}

// ── LOOK module ────────────────────────────────────────────────────────────
Module make_jobs_module() {
    Module m;
    m.name = "jobs";

    // jobs::push($queue, $payload [, $max_retries=3 [, $delay=0]]) → int id
    m.functions["push"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("jobs::push() — (queue, payload [, max_retries [, delay]]) bekler");
        std::string q           = args[0].to_string();
        std::string payload     = args[1].to_string();
        int         max_retries = (args.size() >= 3) ? (int)args[2].to_float() : 3;
        int         delay       = (args.size() >= 4) ? (int)args[3].to_float() : 0;
        return Value((int)JobStore::instance().push(q, payload, max_retries, delay));
    };

    // jobs::next($queue) → assoc | null
    m.functions["next"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::next() — queue bekler");
        return JobStore::instance().next(args[0].to_string());
    };

    // jobs::done($id) → true
    m.functions["done"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::done() — id bekler");
        JobStore::instance().done((int64_t)args[0].to_float());
        return Value(true);
    };

    // jobs::fail($id) → true
    m.functions["fail"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::fail() — id bekler");
        JobStore::instance().fail((int64_t)args[0].to_float());
        return Value(true);
    };

    // jobs::stats($queue) → {pending, processing, done, failed}
    m.functions["stats"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::stats() — queue bekler");
        return JobStore::instance().stats(args[0].to_string());
    };

    // jobs::list($queue, $status [, $limit=100]) → array
    m.functions["list"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("jobs::list() — (queue, status) bekler");
        int limit = (args.size() >= 3) ? (int)args[2].to_float() : 100;
        return JobStore::instance().list(args[0].to_string(), args[1].to_string(), limit);
    };

    // jobs::retry($id) → true
    m.functions["retry"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::retry() — id bekler");
        JobStore::instance().retry((int64_t)args[0].to_float());
        return Value(true);
    };

    // jobs::purge($queue, $status) → int
    m.functions["purge"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("jobs::purge() — (queue, status) bekler");
        return Value((int)JobStore::instance().purge(args[0].to_string(), args[1].to_string()));
    };

    // jobs::recover($queue [, $min_age_seconds=0]) → int (recovered count)
    // Call at startup to rescue processing jobs orphaned by a crash.
    // min_age_seconds=0 resets ALL processing jobs unconditionally.
    // min_age_seconds=300 only resets jobs stuck for 5+ minutes.
    m.functions["recover"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::recover() — queue bekler");
        int min_age = (args.size() >= 2) ? (int)args[1].to_float() : 0;
        int64_t n = JobStore::instance().recover(args[0].to_string(), min_age);
        if (n > 0) {
            look::Logger::instance().log(look::LogLevel::LOG_WARN, "jobs::recover",
                "Crash recovery: " + std::to_string(n) + " processing job(s) → pending");
        }
        return Value((int)n);
    };

    // jobs::failed($queue [, $limit=100]) → alias for jobs::list($queue, "failed")
    m.functions["failed"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("jobs::failed() — queue bekler");
        int limit = (args.size() >= 2) ? (int)args[1].to_float() : 100;
        return JobStore::instance().list(args[0].to_string(), "failed", limit);
    };

    // jobs::worker($queue, $fn) — register a handler for a queue
    // Called at setup time. jobs::run() will invoke these handlers.
    m.functions["worker"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("jobs::worker() — (queue, function) bekler");
        if (args[1].type() != Value::FUNCTION)
            throw std::runtime_error("jobs::worker() — ikinci argüman fonksiyon olmalı");
        JobStore::instance().register_worker(args[0].to_string(), args[1]);
        return Value(true);
    };

    // jobs::run() — ScopeResolution handler in interpreter.cpp intercepts this
    // before module lookup, so no stub needed here.

    return m;
}

} // namespace look

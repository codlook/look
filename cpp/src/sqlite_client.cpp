#include "look/sqlite_client.h"
#include "look/format.h"   // look_format_double (the language's single double format)
#include "sqlite3/sqlite-amalgamation-3530400/sqlite3.h"
#include "look/fiber.h"
#include <atomic>
#include <exception>
#include <functional>
#include <thread>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <set>
#include <map>
#include <memory>
#include <cctype>

namespace look {

// ── Write gate: connections writing to the same file write IN TURN in this process ─────────────
// SQLite allows a single writer; the others sleep at intervals inside busy_timeout and retry.
// That retry is NOT FAIR: with 20 concurrent writers some keep missing their turn and exceed the time
// and returned 500 with "database is locked" (code 5) (measured: with a 2 s timeout 27 errors in 60 s, with 5 s
// still 3 — longest wait 5.5 s). This gate queues the writers inside the process
// by ticket order: nobody loses their turn. Reads (WAL) do not pass through the gate. For other programs
// writing to the same file SQLite's own waiting (busy_timeout) stays in place.
// An open transaction (BEGIN ... COMMIT/ROLLBACK) holds the gate until the transaction ends.
struct SqliteWriteGate {
    std::mutex              m;
    std::condition_variable cv;
    uint64_t                next = 0, serving = 0;
    std::set<uint64_t>      abandoned;   // tickets that timed out and gave up — the turn is skipped
    bool acquire(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lk(m);
        uint64_t t = next++;
        if (cv.wait_for(lk, limit, [&] { return serving == t; })) return true;
        abandoned.insert(t);
        return false;
    }
    // Take without waiting: if the gate is free and nobody is in the queue it takes a ticket.
    bool try_acquire() {
        std::lock_guard<std::mutex> lk(m);
        if (serving != next) return false;
        ++next;
        return true;
    }
    void release() {
        std::lock_guard<std::mutex> lk(m);
        ++serving;
        while (abandoned.erase(serving)) ++serving;
        cv.notify_all();
    }
};
static std::shared_ptr<SqliteWriteGate> sqlite_gate_for(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<SqliteWriteGate>> gates;
    std::lock_guard<std::mutex> lk(mu);
    auto& g = gates[path];
    if (!g) g = std::make_shared<SqliteWriteGate>();
    return g;
}
static bool sqlite_starts_begin(const std::string& sql) {
    size_t i = 0;
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    static const char kw[] = "BEGIN";
    for (int k = 0; k < 5; ++k, ++i)
        if (i >= sql.size() || std::toupper((unsigned char)sql[i]) != kw[k]) return false;
    return true;
}
static constexpr std::chrono::milliseconds SQLITE_GATE_LIMIT{10000};

// If the gate is busy inside a fiber: waiting would hold the thread → the query is handed to the connection's own
// thread (see run()). Discarded before any step has run.
struct SqliteGateBusy {};

void SqliteClient::gate_before(sqlite3_stmt* stmt, const std::string& sql) {
    if (!gate_ || gate_held_) return;
    if (sqlite3_stmt_readonly(stmt) && !sqlite_starts_begin(sql)) return;
    if (in_fiber_) {
        if (!gate_->try_acquire()) throw SqliteGateBusy{};
    } else if (!gate_->acquire(SQLITE_GATE_LIMIT))
        throw std::runtime_error("sqlite: step error: database is locked: waited 10 s for the other "
                                 "writers of this database in this process");
    gate_held_ = true;
}
void SqliteClient::gate_after() {
    // If a transaction is open (autocommit off) the gate is held; released when the transaction ends.
    if (gate_held_ && (!db_ || sqlite3_get_autocommit(db_))) {
        gate_held_ = false;
        gate_->release();
    }
}

// The connection's own thread — usage and rationale are above on_own_thread().
struct SqliteRunner {
    std::mutex              m;
    std::condition_variable cv;
    std::function<void()>   job;
    bool                    stop = false;
    std::thread             th;   // LAST member: the thread starts after the other members are built
    SqliteRunner() : th([this] { loop(); }) {}
    ~SqliteRunner() {
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_one();
        th.join();
    }
    void post(std::function<void()> j) {
        { std::lock_guard<std::mutex> lk(m); job = std::move(j); }
        cv.notify_one();
    }
    void loop() {
        for (;;) {
            std::function<void()> j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || job; });
                if (!job) return;
                j = std::move(job);
                job = nullptr;
            }
            j();
        }
    }
};

// double format → look/format.h look_format_double (the language's single format, including NaN/Inf)

// Trigger ALL of SQLite's lazy one-time global inits serially AT PROCESS START, BEFORE threads
// (see sqlite_client.h). sqlite3_initialize() isInit; opening ":memory:" + randomblob sets up
// the PRNG seed (unixRandomness) + pcache/VFS serially. Otherwise concurrent-open gives TSan a data race on these
// benign double-checked-locking fast-paths (the t4 enforced guard caught it
// in CI: first sqlite3_initialize, then unixRandomness). Serial warm-up → global writes happen-before all
// worker reads → no race. Idempotent.
void sqlite_global_init() {
    sqlite3_initialize();
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) == SQLITE_OK && db) {
        sqlite3_exec(db, "SELECT randomblob(32)", nullptr, nullptr, nullptr);  // PRNG seed
        sqlite3_close(db);
    }
}

SqliteClient::SqliteClient() = default;

SqliteClient::~SqliteClient() {
    close();
}

void SqliteClient::open(const std::string& path) {
    // Fail-loud: if the SQLITE_THREADSAFE compile flag silently turns to =0, the multi-threaded
    // worker pool corrupts SQLite's global statics (see CMakeLists =2). Cheap check.
    if (sqlite3_threadsafe() == 0)
        throw std::runtime_error(
            "sqlite: library built with SQLITE_THREADSAFE=0 — UNSAFE with the multi-threaded worker "
            "pool. CMakeLists must set SQLITE_THREADSAFE=2.");
    if (db_) close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("sqlite: cannot open database '" + path + "': " + err);
    }
    gate_ = (path == ":memory:") ? nullptr : sqlite_gate_for(path);   // ":memory:" is a separate database on every open
    // D-01: busy_timeout FIRST — a concurrent writer waits on BUSY instead of getting an immediate error.
    // WAL allows a single writer; without busy_timeout the second connection in the pool returns at once with "database
    // is locked". Writers in this process already queue at the write gate
    // (above); busy_timeout is now waited only for ANOTHER program writing to the same file
    // — 5000 ms, same as jobs::. Implicit-transaction INSERTs are covered by this;
    // for an explicit BEGIN begin_stmt() issues "BEGIN IMMEDIATE" — SQLITE_BUSY_SNAPSHOT
    // CANNOT be retried with busy_timeout.
    // The wait has the same duration and intervals as busy_timeout(5000) but uses our own handler: inside a
    // fiber it puts the fiber to sleep, not the thread (see on_busy). A long-computing query also
    // yields the turn to other requests every so often inside a fiber (on_progress).
    sqlite3_busy_handler(db_, &SqliteClient::on_busy, this);
    sqlite3_progress_handler(db_, 1000, &SqliteClient::on_progress, this);
    // WAL mode — reduces read/write contention
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // NOTE: synchronous was LEFT at the default (FULL). synchronous=NORMAL silently lowers durability
    // (ACID-D) — on a power failure/OS crash the last commits can be lost
    // (not corruption, LOSS). A web framework's default must not
    // take that without asking the user. Whoever wants perf can later opt in deliberately and documented with DSN ?synchronous=NORMAL.
    // Enable foreign key checking
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

bool SqliteClient::in_transaction() const {
    return db_ != nullptr && (sqlite3_get_autocommit(db_) == 0 || tx_depth > 0);
}

void SqliteClient::close() {
    runner_.reset();   // the connection's thread (if any) — closes while no work is on it
    if (gate_held_) { gate_held_ = false; gate_->release(); }   // an open transaction is rolled back on close
    clear_stmt_cache();   // sqlite3_close does not close the connection while a statement is unfinalized
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ── SQLite inside a fiber: making the fiber wait, not the thread ──────────────
// SQLite is a C library; it holds the calling thread for the duration of a query. In fiber
// dispatch this made ALL requests on that thread wait (1.0.20–1.0.21: during a single long
// query another request waited ~1 s; the old worker model did not wait).
// The query still runs on the calling thread (a fast query pays no extra cost — sending every
// query to another thread slowed fast reads by 2.9×, measured). Three
// moments make the fiber wait:
//   - another program's lock: the busy handler puts the fiber to sleep (on_busy);
//   - long computation: checked every 1000 steps, if 2 ms have elapsed the turn goes to other requests (on_progress);
//   - write gate busy: the query runs on this connection's OWN thread and waits there
//     (on_own_thread). A per-connection thread, not a shared pool: the gate is
//     held for the whole open transaction; in a shared pool, while all threads wait for the gate the
//     COMMIT of the transaction holding the gate could find no free thread → deadlock.
// A connection always belongs to a single request; it is never used from two places at once.
static bool sqlite_in_fiber() {
    return Fiber::current() && get_thread_scheduler();
}

int SqliteClient::on_progress(void* self) {
    auto* c = static_cast<SqliteClient*>(self);
    if (!c->in_fiber_) return 0;
    auto now = std::chrono::steady_clock::now();
    if (now - c->slice_start_ >= std::chrono::milliseconds(2)) {
        Fiber::yield();   // voluntary: the scheduler puts the fiber at the end of the ready queue
        c->slice_start_ = std::chrono::steady_clock::now();
    }
    return 0;
}

int SqliteClient::on_busy(void* self, int count) {
    // SQLite's busy_timeout intervals; total 5000 ms.
    static const int delays[] = {1, 2, 5, 10, 15, 20, 25, 25, 25, 50, 50, 100};
    auto* c = static_cast<SqliteClient*>(self);
    if (count == 0) c->busy_waited_ms_ = 0;
    if (c->busy_waited_ms_ >= 5000) return 0;
    int d = count < 12 ? delays[count] : 100;
    if (d > 5000 - c->busy_waited_ms_) d = 5000 - c->busy_waited_ms_;
    std::shared_ptr<Fiber> sp;
    FiberScheduler* sched = nullptr;
    if (c->in_fiber_) { sched = get_thread_scheduler(); sp = Fiber::current()->shared_from_this_fiber(); }
    if (sp && sched && sched->wait_readable_tmo(sp, -1, d) >= 0) {
        // the fiber slept, the thread went back to other requests
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(d));
    }
    c->busy_waited_ms_ += d;
    c->slice_start_ = std::chrono::steady_clock::now();
    return 1;
}

template <class Fn>
std::vector<DbRow> SqliteClient::run(Fn&& fn) {
    if (!sqlite_in_fiber()) return fn();
    in_fiber_ = true;
    slice_start_ = std::chrono::steady_clock::now();
    try {
        auto rows = fn();
        in_fiber_ = false;
        return rows;
    } catch (const SqliteGateBusy&) {
        in_fiber_ = false;   // no step ran — on its own thread, with the waiting gate
    } catch (...) {
        in_fiber_ = false;
        throw;
    }
    return on_own_thread(fn);
}

template <class Fn>
std::vector<DbRow> SqliteClient::on_own_thread(Fn&& fn) {
    Fiber* cur = Fiber::current();
    FiberScheduler* sched = cur ? get_thread_scheduler() : nullptr;
    std::shared_ptr<Fiber> sp = sched ? cur->shared_from_this_fiber() : nullptr;
    if (!sp) return fn();   // outside a fiber (worker model, jobs, CLI): run directly
    if (!runner_) runner_ = std::make_unique<SqliteRunner>();
    std::vector<DbRow> out;
    std::exception_ptr err;
    std::atomic<bool> done{false};
    runner_->post([&, sched, sp] {
        try { out = fn(); } catch (...) { err = std::current_exception(); }
        done.store(true, std::memory_order_release);
        sched->resume_fiber(sp);
    });
    // Suspend AT LEAST ONCE: even if the work finishes very fast, resume_fiber's wakeup is consumed in this
    // suspension (as pending_resume_ if it comes early). Had we continued without suspending, the leftover
    // wakeup would wake the fiber's NEXT wait (socket/pool) early. Loop: an unexpected wakeup
    // arriving before the work finishes must not touch the out/err on the stack early.
    do { FiberScheduler::suspend_current(); } while (!done.load(std::memory_order_acquire));
    if (err) std::rethrow_exception(err);
    return out;
}

std::vector<DbRow> SqliteClient::query(const std::string& sql) {
    return run([&] { return query_here(sql); });
}

std::vector<DbRow> SqliteClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    return run([&] { return execute_here(sql, params); });
}

std::vector<DbRow> SqliteClient::query_here(const std::string& sql) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_) + " — SQL: " + sql);
    }

    try { gate_before(stmt, sql); } catch (...) { sqlite3_finalize(stmt); throw; }
    struct GateRelease { SqliteClient* c; ~GateRelease() { c->gate_after(); } } gate_release{this};

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
                    dv.str  = look_format_double(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT;
                    break;
                case SQLITE_NULL:
                    dv.str     = "";
                    dv.type    = sqlite_type::NUL;
                    dv.is_null = true;
                    break;
                case SQLITE_BLOB:
                    // return the BLOB as a hex string
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
                        int tb = sqlite3_column_bytes(stmt, i);
                        dv.str  = txt ? std::string(txt, (size_t)tb) : "";  // NUL-safe (length-aware)
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
        throw std::runtime_error(std::string("sqlite: step error: ") + sqlite3_errmsg(db_) + " (code " + std::to_string(sqlite3_extended_errcode(db_)) + ")");
    }

    // update the statistics for INSERT/UPDATE/DELETE
    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }

    return rows;
}

void SqliteClient::clear_stmt_cache() {
    for (auto& kv : stmt_cache_) sqlite3_finalize(kv.second.stmt);
    stmt_cache_.clear();
}

sqlite3_stmt* SqliteClient::take_stmt(const std::string& sql) {
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) {
        sqlite3_stmt* st = it->second.stmt;
        stmt_cache_.erase(it);   // not in the cache while in use
        return st;
    }
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        if (st) sqlite3_finalize(st);
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_));
    }
    return st;
}

void SqliteClient::give_back_stmt(const std::string& sql, sqlite3_stmt* st) {
    if (!st) return;
    sqlite3_reset(st);            // the read ends (an unreset statement would keep it open)
    sqlite3_clear_bindings(st);   // the previous query's values do not stay in the next query
    if (!db_ || stmt_cache_.count(sql)) { sqlite3_finalize(st); return; }   // the same SQL has already been put back
    if (stmt_cache_.size() >= STMT_CACHE_MAX) {   // close the one unused for the longest
        auto old = stmt_cache_.begin();
        for (auto i = stmt_cache_.begin(); i != stmt_cache_.end(); ++i)
            if (i->second.used < old->second.used) old = i;
        sqlite3_finalize(old->second.stmt);
        stmt_cache_.erase(old);
    }
    stmt_cache_.emplace(sql, CachedStmt{st, ++stmt_tick_});
}

std::vector<DbRow> SqliteClient::execute_here(const std::string& sql, const std::vector<DbParam>& params) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = take_stmt(sql);
    // On every exit path (including errors) the statement is reset and returns to the cache.
    struct StmtReturn { SqliteClient* c; const std::string& sql; sqlite3_stmt* st;
                        ~StmtReturn() { c->give_back_stmt(sql, st); } } stmt_return{this, sql, stmt};
    int rc = SQLITE_OK;

    // Placeholder COUNT check (fail-loud, like Go db.Query) — SQLite treats an unbound '?'
    // silently as NULL; a missing/extra parameter would be a hidden bug. SQLite's OWN count
    // (excluding '?' inside backtick-ident, string, comment) → no blind spot of the old dialect parser.
    int want = sqlite3_bind_parameter_count(stmt);
    if (want != (int)params.size()) {
        // (the statement returns to the cache via StmtReturn)
        throw std::runtime_error("db: parameter count mismatch — SQL " + std::to_string(want) +
            " placeholder ('?') iceriyor, " + std::to_string(params.size()) + " parameters given.");
    }

    // Parameter binding — a real prepared statement, no string escaping
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

    gate_before(stmt, sql);   // if it throws the statement returns to the cache via StmtReturn
    struct GateRelease { SqliteClient* c; ~GateRelease() { c->gate_after(); } } gate_release{this};

    std::vector<DbRow> rows;
    // The column count is read AFTER the first step: if the schema changed (ALTER TABLE …) SQLite re-prepares the cached
    // statement inside the first sqlite3_step and the columns change — a previously read count
    // would be that of the old schema (SELECT * would not return the new column; tests/sqlite_stmt_cache_test.sh).
    int col_count = -1;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (col_count < 0) col_count = sqlite3_column_count(stmt);
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
                    dv.str  = look_format_double(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT; break;
                case SQLITE_NULL:
                    dv.is_null = true; dv.type = sqlite_type::NUL; break;
                default: {
                    const char* txt = (const char*)sqlite3_column_text(stmt, i);
                        int tb = sqlite3_column_bytes(stmt, i);
                        dv.str  = txt ? std::string(txt, (size_t)tb) : "";  // NUL-safe (length-aware)
                    dv.type = sqlite_type::TEXT; break;
                }
            }
            row.push_back({col_name, dv});
        }
        rows.push_back(std::move(row));
    }
    sqlite3_reset(stmt);   // the read used to end exactly here with finalize — the same moment (error code/message preserved)

    // rc-check — SYMMETRIC with the one in query() (line 139). It was MISSING before: on a constraint
    // violation sqlite3_step returns SQLITE_CONSTRAINT (not ROW) → the loop exits, the row
    // is rejected but NO exception was thrown → db::exec silently returned success (UNIQUE/
    // NOT NULL/FK/CHECK were silently swallowed, a data-integrity trap). Now the same
    // contract as query(): fail-loud.
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("sqlite: step error: ") + sqlite3_errmsg(db_) + " (code " + std::to_string(sqlite3_extended_errcode(db_)) + ")");
    }

    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }
    return rows;
}

} // namespace look

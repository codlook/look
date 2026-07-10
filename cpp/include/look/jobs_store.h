#pragma once
#include "interpreter.h"
struct sqlite3;  // forward declare — full definition in jobs_stdlib.cpp
#include <string>
#include <mutex>
#include <vector>
#include <map>

namespace look {

// Persistent, SQLite-backed job queue.
// Survives process restart. DB path: env JOBS_DB or "jobs.db".
class JobStore {
public:
    static JobStore& instance();

    // Push a job. max_retries=3, delay_seconds=0 (run immediately).
    int64_t push(const std::string& queue, const std::string& payload_json,
                 int max_retries, int delay_seconds);

    // Claim next READY pending job (run_after <= now) → processing. Null if empty.
    Value next(const std::string& queue);

    // Mark processing job as done.
    void done(int64_t id);

    // Mark as failed. retry_count < max_retries → pending; else → failed.
    void fail(int64_t id);

    // Stats: {pending, processing, done, failed}
    Value stats(const std::string& queue);

    // List jobs by status.
    Value list(const std::string& queue, const std::string& status, int limit);

    // Force failed → pending (manual retry).
    void retry(int64_t id);

    // Delete jobs by status.
    int64_t purge(const std::string& queue, const std::string& status);

    // Crash recovery: processing → pending for jobs older than min_age_seconds.
    // Call at startup. min_age_seconds=0 resets ALL processing jobs unconditionally.
    int64_t recover(const std::string& queue, int min_age_seconds);

    // Register a LOOK function handler for a queue.
    void register_worker(const std::string& queue, Value fn);

    // Get registered workers (for jobs::run()).
    const std::vector<std::pair<std::string, Value>>& workers() const { return workers_; }

    void init(const std::string& db_path);
    ~JobStore();

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
    bool initialized_ = false;
    std::vector<std::pair<std::string, Value>> workers_;

    void ensure_init();
    void create_schema();
    int64_t now_ts();
};

Module make_jobs_module();

} // namespace look

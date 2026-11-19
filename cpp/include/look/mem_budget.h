#pragma once
// Memory budget — one request cannot kill the whole process (Go GOMEMLIMIT model, a single setting).
//
// OLD: there was no memory limit per request or for the process as a whole. A SINGLE request growing without bound
// (a list/string growing in a loop) killed the server in a 512 MB container in under 1 s
// (the kernel's OOM killer, rc=137 — measured); after that no request got a response.
//
// Model:
//   * Process limit: LOOK_MEMORY_LIMIT (Go's GOMEMLIMIT format: 512MiB, 1GiB, bytes). If not given it is
//     automatic: 80% of the container (cgroup memory.max), 1 GiB if there is no container limit. This is the only
//     memory setting in the settings list; the request budget is NOT a separate setting.
//   * Request budget: automatic — 256 MiB, never more than half of the process limit.
//   * Counting: every C++ allocation (operator new/delete) is written to the current request's account and to the
//     process counter. The request account is thread-local, and on a fiber switch it is switched to the fiber's account;
//     a free never takes the account below zero (shared data allocated by another request).
//     The process counter is written in batches of 1 MiB per thread (no atomic contention).
//   * The error is raised NOT in the MIDDLE of an allocation but at the VM's safe points (loop back-edge, call):
//     C++ code is not cut halfway. An allocation that exceeds the limit resets the give-up-turn counter → the next
//     safe point checks immediately. Built-ins that make one large allocation ask first with mem_reserve().
//   * When 90% of the process limit is full new requests get 503 (those in progress continue); at 100% the request using the
//     MOST memory ends with an explicit error (like the Linux OOM killer — not the small request that happens to allocate at that moment).
//   * The memory stop CANNOT BE CAUGHT by LOOK try/catch (out-of-memory cannot be caught in Go/Rust either):
//     if it could be caught, a `try { … } catch` loop would swallow the stop and keep growing with small allocations.
//   * Response: a request that exceeds its own budget gets 500 (the application's fault); the largest request stopped because the server is full
//     gets 503 + Retry-After (the client may retry) — the same code as one refused at 90%.
//   * Command line (lk): NO default limit (like Go — whoever runs the script knows what they are doing). The process limit applies only if
//     LOOK_MEMORY_LIMIT is given explicitly; the request budget never applies.
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace look {

// Memory stop — the VM try/catch bridge does not deliver this to LOOK code (it passes through like exit).
// server_full: stopped because the server is full (503); false: exceeded its own request budget (500).
class MemoryLimitError : public std::runtime_error {
public:
    MemoryLimitError(const std::string& m, bool full) : std::runtime_error(m), server_full(full) {}
    bool server_full;
};

struct MemAccount {
    std::atomic<int64_t> bytes{0};      // single writer: the account's request (thread / fiber)
    int64_t              limit = 0;     // request budget
    std::atomic<bool>    kill{false};   // process limit: the largest request was chosen
    MemAccount*          next = nullptr;   // list of active accounts (under lock)
    MemAccount*          prev = nullptr;
};

// Once at process start (web modes): determines the limit and opens the budget. An invalid LOOK_MEMORY_LIMIT →
// false + why (the process prints the reason and exits). describe: the log line ("memory limit 1 GiB (no …)").
bool mem_budget_configure(std::string& why, std::string& describe);
// Command line (lk): the process limit only if LOOK_MEMORY_LIMIT is given (no request budget, no automatic
// value). Invalid value → false + why.
bool mem_budget_configure_cli(std::string& why);
bool mem_budget_enabled();
int64_t mem_process_limit();      // bytes
int64_t mem_request_limit();      // bytes
int64_t mem_process_bytes();      // live bytes counted (approximate: ≤ 1 MiB difference per thread)

// Can a new request be accepted (below 90%). If not the caller returns 503.
bool mem_admit();

// Request scope: registers the account, binds it to the thread (and to the fiber if inside one); the destructor undoes it.
class MemScope {
public:
    MemScope();
    ~MemScope();
    MemScope(const MemScope&) = delete;
    MemScope& operator=(const MemScope&) = delete;
    MemAccount& account() { return acct_; }
private:
    MemAccount  acct_;
    MemAccount* prev_ = nullptr;
};

// Safe-point check (VM): throws LookVmError if the request budget was exceeded or this request was chosen at the process limit.
// The fast path is in the caller (the fiber.h preempt counter) — this is called only on the slow path.
void mem_check();
// BEFORE a single large allocation (string::repeat, file::read, json::decode, range …): if adding bytes would exceed the request
// budget or the process limit, an error (the allocation is never made). what: "string::repeat()".
void mem_reserve(int64_t bytes, const char* what);

// Fiber switch (fiber_posix.cpp / fiber_win.cpp): the thread's current account.
MemAccount* mem_current_account() noexcept;
void        mem_set_current_account(MemAccount* a) noexcept;

std::string mem_format_bytes(int64_t b);   // "256 MiB"

} // namespace look

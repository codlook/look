#include "look/runtime_init.h"
#include "look/http_client.h"    // configure_system_ca_bundle
#include "look/sqlite_client.h"  // sqlite_global_init
#ifndef _WIN32
#  include <csignal>
#endif

namespace look {

// Once at process start, BEFORE the WORKER THREADS. Order does not matter; both are
// idempotent + once-only inits that must run before the threads.
void runtime_init() {
#ifndef _WIN32
    // Writing to a closed socket raises SIGPIPE; the default behavior KILLS THE PROCESS. Only
    // `--mode http` ignored it: when MySQL closed an idle pooled connection (wait_timeout,
    // restart, KILL) the next write killed lk-fcgi in FastCGI mode (the Plesk default) and
    // `lk` (exit 141). All three entry points call this. A write now
    // returns an EPIPE error; DB clients also write with MSG_NOSIGNAL. (`lk` still
    // terminates when its own stdout is closed -- see main.cpp PipeGuard.)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    configure_system_ca_bundle();  // static OpenSSL: find the system CA (no-op on Windows -- Schannel)
    sqlite_global_init();          // serialize SQLite's lazy global inits (isInit/PRNG) (t4 race)
}

} // namespace look

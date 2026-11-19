#pragma once
// Graceful shutdown (lk-fcgi: HTTP mode, FastCGI TCP and pipe modes).
//
// OLD: there was no handler for SIGTERM/SIGINT — systemd stop/restart (Plesk release switch, deploy) killed the process
// instantly: requests in progress were cut, the client saw a half response or a connection error,
// database connections were left unclosed.
//
// Model (Go http.Server.Shutdown): on the first signal accepting new connections stops, idle
// connections are closed, requests in progress finish (answered with "Connection: close"), the database
// pools are closed, the process exits with 0. The upper bound is a fixed 30 s (no setting — a single option); if a request
// does not finish the reason is written and it exits with 1. A second signal exits immediately.
//
// Signals are received in a separate thread with sigwait: install() must be called with NO thread
// opened in the process yet (the blocking mask is inherited by threads; otherwise the signal
// lands on a random one such as the database thread and the default action kills the process).
#include <functional>

namespace look::shutdown {

// At process start (main's first job). Windows: does nothing (the default behaviour continues).
void install();

// The job to call when shutdown starts (registered after the server is set up). For a signal that arrives BEFORE registration
// there is no request in progress: the process exits immediately with finish(0).
void on_begin(std::function<void()> fn);

// Was shutdown requested (loops check this so as not to take new work).
bool requested();

// Finish: closes the database pools, writes to the log, exits (destructors do not run — so that a static
// destructor waiting for a lock at exit does not hang the process).
[[noreturn]] void finish(int code);

} // namespace look::shutdown

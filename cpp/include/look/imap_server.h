#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "look/event_loop.h"

// ── LOOK IMAP4rev1 Server (RFC 3501) ──────────────────────────────────────────
//
// The SMTP server writes mail to the Maildir (deliver_maildir); IMAP reads the same Maildir.
// A shared store — no migration, dovecot-compatible, crash-safe.
//   Maildir layout: <base>/<mailbox>/{new,cur,tmp}/
//
// Milestone plan (each with its own security verification):
//   M1: greeting · CAPABILITY · LOGIN · LOGOUT · NOOP        (this release)
//   M2: SELECT/EXAMINE · LIST/LSUB · STATUS
//   M3: FETCH · STORE · EXPUNGE
//   M4: SEARCH · APPEND · UID variants
//   M5: STARTTLS · IMAPS(993)
//   M6: IDLE (push)
//   M7: security round (fuzzing/UBSan) + performance (concurrent IDLE)
//
// Security principles (from the start, set via env):
//   LOOK_IMAP_MAX_CONN     — concurrent sessions      (default: 1000)
//   LOOK_IMAP_MAX_LINE     — command line bytes      (default: 8 KB)
//   LOOK_IMAP_MAX_LITERAL  — {N} literal size     (default: 32 MB)
//                            — RESP2 read_bulk lesson: the literal size
//                              is network-controlled → unbounded alloc = OOM DoS.
//   LOOK_IMAP_MAX_ERRORS   — cut after errors       (default: 5)
//   LOOK_IMAP_AUTH_DELAY   — failed-login delay (slows brute force)
//
// Authentication: the same PBKDF2-SHA256 hash store as SMTP (auth callback).
// Mailbox isolation: each user reaches only their own Maildir; mailbox
// names are verified inside the root directory with canonical-path (no path traversal).

namespace look {

// IMAP authentication result — the callback returns this.
struct ImapAuthResult {
    bool        ok = false;
    std::string maildir_path;   // the user's Maildir root directory (if ok)
};

// User/password → auth result. Uses SMTP's PBKDF2 store.
using ImapAuthHandler =
    std::function<ImapAuthResult(const std::string& user, const std::string& pass)>;

// ── IMAP server ───────────────────────────────────────────────────────────────
class ImapServer {
public:
    // port_imap  : :143 (upgraded with STARTTLS)
    // port_imaps : :993 (implicit TLS — 0 = off until M5)
    // workers    : session handler thread pool
    ImapServer(int port_imap, int port_imaps, int workers, ImapAuthHandler auth);
    ~ImapServer();

    ImapServer(const ImapServer&)            = delete;
    ImapServer& operator=(const ImapServer&) = delete;

    bool start();   // open the listeners; true if it STARTED listening (prevents a misleading "started" log)
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace look

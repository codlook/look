#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "look/event_loop.h"

namespace look {

// ── Parsed inbound message ────────────────────────────────────────────────────

enum class SpfResult { PASS, SOFTFAIL, FAIL, NEUTRAL, NONE };

struct SmtpMessage {
    std::string         mail_from;      // MAIL FROM:<addr>
    std::vector<std::string> rcpt_to;  // RCPT TO:<addr> list
    std::string         data;           // raw RFC 5322 message body
    std::string         remote_ip;      // connecting client IP
    bool                tls           = false; // STARTTLS was negotiated
    bool                authenticated = false; // AUTH PLAIN succeeded on :587
    SpfResult           spf = SpfResult::NONE; // result of SPF check
    bool                dkim_ok = false;       // DKIM-Signature verified
};

// ── Delivery handler ──────────────────────────────────────────────────────────
// Called on a worker thread for each fully received message.
// Return true  → 250 OK sent to client
// Return false → 550 rejected
using SmtpHandler = std::function<bool(const SmtpMessage&)>;

// ── SMTP server ───────────────────────────────────────────────────────────────
// Listens on one or more ports (25 / 587 / 465) using the shared EventLoop.
// Session state is managed per-connection via SmtpSession (smtp_server.cpp).
// TLS is upgrade-on-demand (STARTTLS on :25/:587) or implicit (:465, future).
//
// DoS limits (all configurable via env vars):
//   LOOK_SMTP_MAX_CONN     — max concurrent sessions   (default: 1000)
//   LOOK_SMTP_MAX_MSG_SIZE — max message bytes         (default: 25 MB)
//   LOOK_SMTP_MAX_RCPT     — max recipients per mail   (default: 100)
//   LOOK_SMTP_MAX_ERRORS   — errors before disconnect  (default: 5)
//   LOOK_SMTP_BANNER       — EHLO banner hostname      (default: "localhost")

class SmtpServer {
public:
    // port_smtp  : :25  (MTA-to-MTA, requires root or CAP_NET_BIND_SERVICE)
    // port_sub   : :587 (authenticated submission)
    // port_smtps : :465 (implicit TLS — 0 = disabled until TLS wired)
    // workers    : handler thread pool size
    SmtpServer(int port_smtp, int port_sub, int port_smtps,
               int workers, SmtpHandler handler);
    ~SmtpServer();

    void run();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Maildir helper ────────────────────────────────────────────────────────────
// deliver_maildir() writes a message to Maildir format under base_dir/<mailbox>/.
// Returns the filename written, or throws on I/O error.
std::string deliver_maildir(const std::string& base_dir,
                            const std::string& mailbox,
                            const SmtpMessage& msg);

// ── Ortak mail kimlik doğrulama (SMTP AUTH ile aynı model) ───────────────────
// mail_users tablosuna (pbkdf2 constant-time) veya dev fallback'e karşı doğrular;
// IMAP LOGIN ve diğer mail servisleri paylaşır — kripto tek yerde.
//   Backend seçimi env ile:
//     LOOK_MAIL_USER_DB / LOOK_SMTP_USER_DB  → mysql://... (mail_users tablosu)
//     LOOK_MAIL_USER + LOOK_MAIL_PASS        → tek kullanıcı (dev/basit deployment)
//   maildir_out = LOOK_MAIL_DIR/<user>/inbox  (SMTP teslimatıyla aynı yer).
// Dönen: true = doğrulandı (maildir_out doldurulur), false = ret.
bool mail_user_auth(const std::string& user, const std::string& pass,
                    std::string& maildir_out);

} // namespace look

// mail_stdlib.cpp — mail:: module
// Minimal external runtime dependencies: the HTTP-API providers reuse http_client
// (Schannel/OpenSSL); the smtp provider speaks SMTP directly over OpenSSL (POSIX).
// Provider selection via env MAIL_PROVIDER (mailgun|sendgrid|postmark|smtp).
// All providers share the same LOOK API — switch provider without code change.
#include "look/mail.h"
#include "look/smtp_server.h"
#include "look/http_client.h"
#include "look/logger.h"
#include <stdexcept>
#include <map>
#include <cstdlib>
#include <cstring>

// SMTP transport needs raw sockets + OpenSSL at global scope (system headers must
// not be pulled inside namespace look, or ::socket/::connect resolve into it).
#if defined(__linux__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <atomic>
#include <ctime>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace look {

// ── base64 (for Basic auth) ───────────────────────────────────────────────────
static std::string base64_encode(const std::string& in) {
    static const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ── url encode (form body helper) ────────────────────────────────────────────
static std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ── env helper ────────────────────────────────────────────────────────────────
static std::string env_get(const char* key, const char* def = "") {
    const char* v = std::getenv(key);
    return v ? v : def;
}

// ── header-injection guard (gate #4) ──────────────────────────────────────────
// A recipient/subject/from is a single header line: CR/LF in it would let an
// attacker inject extra headers ("Subject: hi\r\nBcc: evil@x"). Strip both. Kept
// platform-independent so every provider (SMTP and the HTTP APIs) is covered.
static std::string hdr_clean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) { if (c != '\r' && c != '\n') out.push_back(c); }
    return out;
}
static bool has_high_byte(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}
// MIME "encoded-word" for a non-ASCII header value (e.g. a Turkish subject).
static std::string mime_encoded_word(const std::string& s) {
    return "=?UTF-8?B?" + base64_encode(s) + "?=";
}

// ── MailResult ────────────────────────────────────────────────────────────────
struct MailResult {
    bool        ok      = false;
    int         status  = 0;
    std::string message;
};

// ── Mailgun ───────────────────────────────────────────────────────────────────
// POST https://api.mailgun.net/v3/{domain}/messages (form-encoded)
static MailResult send_mailgun(const std::string& api_key,
                                const std::string& from,
                                const std::string& to,
                                const std::string& subject,
                                const std::string& text,
                                const std::string& html,
                                const std::string& domain)
{
    MailResult r;
    std::string url = "https://api.mailgun.net/v3/" + domain + "/messages";

    std::string body;
    body += "from="    + url_encode(from)    + "&";
    body += "to="      + url_encode(to)      + "&";
    body += "subject=" + url_encode(subject) + "&";
    if (!html.empty())
        body += "html=" + url_encode(html) + "&";
    body += "text=" + url_encode(text.empty() ? subject : text);

    std::map<std::string, std::string> hdrs;
    hdrs["Authorization"] = "Basic " + base64_encode("api:" + api_key);
    hdrs["Content-Type"]  = "application/x-www-form-urlencoded";

    HttpOptions opts; opts.timeout_ms = 15000;
    HttpClientResponse resp = http_request("POST", url, body, hdrs, opts);

    r.status  = resp.status;
    r.ok      = (resp.status == 200);
    r.message = resp.error.empty() ? resp.body : resp.error;
    return r;
}

// ── SendGrid ──────────────────────────────────────────────────────────────────
// POST https://api.sendgrid.com/v3/mail/send (JSON)
static MailResult send_sendgrid(const std::string& api_key,
                                 const std::string& from,
                                 const std::string& to,
                                 const std::string& subject,
                                 const std::string& text,
                                 const std::string& html)
{
    MailResult r;
    // Build minimal JSON manually (no json:: dep from C++ side)
    // JSON escape: tüm kontrol karakterleri dahil (RFC 7159 §7)
    auto esc = [](const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) {
                char buf[7]; snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else out += (char)c;
        }
        return out;
    };

    std::string content_arr;
    if (!html.empty())
        content_arr += "{\"type\":\"text/html\",\"value\":\"" + esc(html) + "\"},";
    content_arr += "{\"type\":\"text/plain\",\"value\":\"" + esc(text.empty() ? subject : text) + "\"}";

    std::string body =
        "{\"personalizations\":[{\"to\":[{\"email\":\"" + esc(to) + "\"}]}],"
        "\"from\":{\"email\":\"" + esc(from) + "\"},"
        "\"subject\":\"" + esc(subject) + "\","
        "\"content\":[" + content_arr + "]}";

    std::map<std::string, std::string> hdrs;
    hdrs["Authorization"] = "Bearer " + api_key;
    hdrs["Content-Type"]  = "application/json";

    HttpOptions opts; opts.timeout_ms = 15000;
    HttpClientResponse resp = http_request("POST", "https://api.sendgrid.com/v3/mail/send",
                                     body, hdrs, opts);
    r.status  = resp.status;
    r.ok      = (resp.status == 202);
    r.message = resp.error.empty() ? resp.body : resp.error;
    return r;
}

// ── Postmark ──────────────────────────────────────────────────────────────────
// POST https://api.postmarkapp.com/email (JSON)
static MailResult send_postmark(const std::string& api_key,
                                 const std::string& from,
                                 const std::string& to,
                                 const std::string& subject,
                                 const std::string& text,
                                 const std::string& html)
{
    MailResult r;
    // JSON escape: tüm kontrol karakterleri dahil (RFC 7159 §7)
    auto esc = [](const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) {
                char buf[7]; snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else out += (char)c;
        }
        return out;
    };

    std::string body = "{\"From\":\"" + esc(from) + "\","
                       "\"To\":\"" + esc(to) + "\","
                       "\"Subject\":\"" + esc(subject) + "\"";
    if (!text.empty()) body += ",\"TextBody\":\"" + esc(text) + "\"";
    if (!html.empty()) body += ",\"HtmlBody\":\"" + esc(html) + "\"";
    body += "}";

    std::map<std::string, std::string> hdrs;
    hdrs["X-Postmark-Server-Token"] = api_key;
    hdrs["Content-Type"]            = "application/json";
    hdrs["Accept"]                  = "application/json";

    HttpOptions opts; opts.timeout_ms = 15000;
    HttpClientResponse resp = http_request("POST", "https://api.postmarkapp.com/email",
                                     body, hdrs, opts);
    r.status  = resp.status;
    r.ok      = (resp.status == 200);
    r.message = resp.error.empty() ? resp.body : resp.error;
    return r;
}

// ── SMTP transport ────────────────────────────────────────────────────────────
// Outbound SMTP relay: the transport a shared-hosting site actually has (cPanel /
// Yandex SMTP), where an HTTP-API account (Mailgun/SendGrid) does not exist. This
// is one more MailResult-returning branch behind mail::send — same API, no new
// namespace (smtp:: is already the server side). POSIX + OpenSSL only, mirroring
// the SMTP/IMAP *server* stack which is likewise Linux/macOS-only.
#if defined(__linux__) || defined(__APPLE__)
// wrap base64 at 76 columns (RFC 2045 body)
static std::string b64_wrap(const std::string& raw) {
    std::string b = base64_encode(raw), out;
    for (size_t i = 0; i < b.size(); i += 76) { out += b.substr(i, 76); out += "\n"; }
    return out;
}
static std::string smtp_uniq() {
    static std::atomic<unsigned long> ctr{0};
    return std::to_string((unsigned long)time(nullptr)) + "."
         + std::to_string((unsigned long)++ctr) + "."
         + std::to_string((unsigned long)getpid());
}
// RFC 5322 date with fixed English day/month names (never the server locale).
static std::string smtp_date() {
    time_t t = time(nullptr);
    struct tm g; gmtime_r(&t, &g);
    static const char* dow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char b[64];
    snprintf(b, sizeof(b), "%s, %02d %s %d %02d:%02d:%02d +0000",
             dow[g.tm_wday], g.tm_mday, mon[g.tm_mon], g.tm_year + 1900,
             g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

// A plain-fd-or-TLS connection with line-oriented read and reply-code parsing.
struct SmtpConn {
    int      fd  = -1;
    SSL*     ssl = nullptr;
    SSL_CTX* ctx = nullptr;
    ~SmtpConn() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        if (fd >= 0) ::close(fd);
    }
    bool write_all(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            int n = ssl ? SSL_write(ssl, s.data() + off, (int)(s.size() - off))
                        : (int)::send(fd, s.data() + off, s.size() - off, 0);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }
    bool read_line(std::string& line) {
        line.clear();
        char c;
        for (;;) {
            int n = ssl ? SSL_read(ssl, &c, 1) : (int)::recv(fd, &c, 1, 0);
            if (n <= 0) return false;                       // error / timeout / EOF
            if (c == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
            line.push_back(c);
            if (line.size() > 8192) return false;           // runaway line guard
        }
    }
};

// read one (possibly multi-line) reply; return the 3-digit code, -1 on error.
static int smtp_reply(SmtpConn& c, std::string& text) {
    text.clear();
    for (;;) {
        std::string line;
        if (!c.read_line(line)) return -1;
        text += line + "\n";
        if (line.size() < 3) return -1;
        if (line.size() == 3 || line[3] == ' ')             // final line (not "NNN-")
            return (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
    }
}
// send cmd (skip if empty), read reply, accept `ok` or `ok2` (0 = no alt).
static bool smtp_cmd(SmtpConn& c, const std::string& cmd, int ok, int ok2, std::string& reply) {
    if (!cmd.empty() && !c.write_all(cmd + "\r\n")) return false;
    int code = smtp_reply(c, reply);
    return code == ok || (ok2 != 0 && code == ok2);
}

// client-side TLS handshake on c.fd, verifying the peer against the system CA
// bundle and enforcing the hostname (gate #2: verification on by default).
static bool smtp_tls(SmtpConn& c, const std::string& host) {
    c.ctx = SSL_CTX_new(TLS_client_method());
    if (!c.ctx) return false;
    SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(c.ctx);                // reads SSL_CERT_FILE/DIR
    c.ssl = SSL_new(c.ctx);
    if (!c.ssl) return false;
    SSL_set_fd(c.ssl, c.fd);
    SSL_set_tlsext_host_name(c.ssl, host.c_str());          // SNI
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_set1_host(c.ssl, host.c_str());                     // reject a mismatched cert
#endif
    return SSL_connect(c.ssl) == 1;
}

static bool smtp_connect(SmtpConn& c, const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0) return false;
    int s = -1;
    for (auto* p = res; p; p = p->ai_next) {
        s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        struct timeval tv; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(s, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(s); s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) return false;
    c.fd = s;
    return true;
}

// normalize line endings to '\n', then CRLF + dot-stuff for the DATA phase.
static std::string smtp_encode_data(const std::string& raw) {
    std::string lf;                                          // collapse \r\n and bare \r to \n
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\r') { lf.push_back('\n'); if (i + 1 < raw.size() && raw[i+1] == '\n') i++; }
        else lf.push_back(raw[i]);
    }
    std::string out; out.reserve(lf.size() + 64);
    bool linestart = true;
    for (char ch : lf) {
        if (linestart && ch == '.') out.push_back('.');     // dot-stuffing
        if (ch == '\n') { out += "\r\n"; linestart = true; }
        else { out.push_back(ch); linestart = false; }
    }
    if (out.size() < 2 || out[out.size()-2] != '\r' || out.back() != '\n') out += "\r\n";
    return out;
}

// assemble an RFC 5322 message; bodies are base64 (safe for 8-bit UTF-8 and long lines).
static std::string smtp_build(const std::string& from, const std::string& to,
                              const std::string& subject, const std::string& text,
                              const std::string& html) {
    std::string dom = "localhost";
    auto at = from.rfind('@'); if (at != std::string::npos) dom = from.substr(at + 1);
    std::string subj = has_high_byte(subject) ? mime_encoded_word(subject) : subject;
    std::string m;
    m += "Date: " + smtp_date() + "\n";
    m += "From: " + from + "\n";
    m += "To: " + to + "\n";
    m += "Subject: " + subj + "\n";
    m += "Message-ID: <" + smtp_uniq() + "@" + dom + ">\n";
    m += "MIME-Version: 1.0\n";
    if (!html.empty() && !text.empty()) {
        std::string b = "==look_" + smtp_uniq() + "==";
        m += "Content-Type: multipart/alternative; boundary=\"" + b + "\"\n\n";
        m += "--" + b + "\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: base64\n\n" + b64_wrap(text) + "\n";
        m += "--" + b + "\nContent-Type: text/html; charset=UTF-8\nContent-Transfer-Encoding: base64\n\n"  + b64_wrap(html) + "\n";
        m += "--" + b + "--\n";
    } else if (!html.empty()) {
        m += "Content-Type: text/html; charset=UTF-8\nContent-Transfer-Encoding: base64\n\n" + b64_wrap(html);
    } else {
        m += "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: base64\n\n" + b64_wrap(text.empty() ? subject : text);
    }
    return m;
}

static MailResult send_smtp(const std::string& from, const std::string& to,
                            const std::string& subject, const std::string& text,
                            const std::string& html) {
    MailResult r;
    std::string host = env_get("SMTP_HOST", "");
    int         port = atoi(env_get("SMTP_PORT", "587").c_str());
    std::string user = env_get("SMTP_USER", "");
    std::string pass = env_get("SMTP_PASS", "");
    std::string tls  = env_get("SMTP_TLS",  "");            // "" | starttls | implicit | none

    bool implicit = (tls == "implicit") || (tls.empty() && port == 465);
    bool starttls = !implicit && (tls != "none");
    if (tls == "none") {
        // gate #2: refusing TLS ships the password in clear text — say so, loudly.
        Logger::instance().log(LogLevel::LOG_WARN, "mail::send",
            "SMTP_TLS=none — credentials and message sent in clear text to " + host);
    }

    SmtpConn c;
    if (!smtp_connect(c, host, port, 15000)) {
        r.message = "SMTP connect failed: " + host + ":" + std::to_string(port);
        return r;
    }
    std::string reply;
    if (implicit && !smtp_tls(c, host)) { r.message = "SMTP implicit-TLS handshake failed for " + host; return r; }
    if (!smtp_cmd(c, "", 220, 0, reply)) { r.message = "SMTP greeting refused: " + reply; return r; }

    std::string ehlo = "EHLO " + env_get("SMTP_EHLO", "look.localhost");
    if (!smtp_cmd(c, ehlo, 250, 0, reply)) { r.message = "EHLO refused: " + reply; return r; }
    if (starttls) {
        if (!smtp_cmd(c, "STARTTLS", 220, 0, reply)) { r.message = "STARTTLS refused: " + reply; return r; }
        if (!smtp_tls(c, host))                       { r.message = "SMTP STARTTLS handshake failed for " + host; return r; }
        if (!smtp_cmd(c, ehlo, 250, 0, reply))        { r.message = "EHLO refused after STARTTLS: " + reply; return r; }
    }
    if (!user.empty()) {
        // AUTH LOGIN — credentials are never logged (gate #1); a failure says only "failed".
        if (!smtp_cmd(c, "AUTH LOGIN", 334, 0, reply))      { r.message = "SMTP AUTH LOGIN not offered: " + reply; return r; }
        if (!smtp_cmd(c, base64_encode(user), 334, 0, reply)) { r.message = "SMTP authentication failed"; return r; }
        if (!smtp_cmd(c, base64_encode(pass), 235, 0, reply)) { r.message = "SMTP authentication failed"; return r; }
    }
    if (!smtp_cmd(c, "MAIL FROM:<" + from + ">", 250, 0, reply))   { r.message = "MAIL FROM refused: " + reply; return r; }
    if (!smtp_cmd(c, "RCPT TO:<"  + to   + ">", 250, 251, reply))  { r.message = "RCPT TO refused: " + reply; return r; }
    if (!smtp_cmd(c, "DATA", 354, 0, reply))                       { r.message = "DATA refused: " + reply; return r; }

    std::string payload = smtp_encode_data(smtp_build(from, to, subject, text, html)) + ".\r\n";
    if (!c.write_all(payload)) { r.message = "SMTP write failed during DATA"; return r; }
    int code = smtp_reply(c, reply);
    if (code != 250) { r.message = "message rejected: " + reply; return r; }

    smtp_cmd(c, "QUIT", 221, 0, reply);                     // best-effort
    r.ok = true; r.status = 250; r.message = "accepted by " + host;
    return r;
}
#else   // Windows / other: no OpenSSL here (Schannel build), same as the server stack
static MailResult send_smtp(const std::string&, const std::string&,
                            const std::string&, const std::string&, const std::string&) {
    MailResult r;
    r.message = "mail:: — the smtp provider needs the Linux/macOS build; "
                "use mailgun/sendgrid/postmark on this platform";
    return r;
}
#endif

// ── dispatch ──────────────────────────────────────────────────────────────────
static MailResult dispatch_send(const std::string& to_in,
                                 const std::string& subject_in,
                                 const std::string& text,
                                 const std::string& html,
                                 const std::string& from_override)
{
    std::string provider = env_get("MAIL_PROVIDER", "mailgun");
    std::string from_raw = from_override.empty() ? env_get("MAIL_FROM", "") : from_override;

    // gate #4: recipient/subject/from are single header lines — strip any CR/LF an
    // attacker could smuggle in through form input. Bodies keep their newlines.
    std::string to      = hdr_clean(to_in);
    std::string subject = hdr_clean(subject_in);
    std::string from    = hdr_clean(from_raw);

    if (from.empty())
        throw std::runtime_error("mail:: — MAIL_FROM env variable is missing (or specify the from parameter)");

    // SMTP relay: no API key; needs a host. One more branch, same MailResult.
    if (provider == "smtp") {
        if (env_get("SMTP_HOST", "").empty())
            throw std::runtime_error("mail:: — SMTP_HOST env variable is required for the smtp provider");
        return send_smtp(from, to, subject, text, html);
    }

    std::string api_key = env_get("MAIL_API_KEY", "");
    if (api_key.empty())
        throw std::runtime_error("mail:: — MAIL_API_KEY env variable is missing");

    if (provider == "mailgun") {
        std::string domain = env_get("MAIL_DOMAIN", "");
        if (domain.empty()) {
            // Extract domain from from address: "name@domain.com" → "domain.com"
            auto at = from.rfind('@');
            if (at != std::string::npos) domain = from.substr(at + 1);
        }
        if (domain.empty())
            throw std::runtime_error("mail:: — MAIL_DOMAIN is required for Mailgun");
        return send_mailgun(api_key, from, to, subject, text, html, domain);
    }

    if (provider == "sendgrid")
        return send_sendgrid(api_key, from, to, subject, text, html);

    if (provider == "postmark")
        return send_postmark(api_key, from, to, subject, text, html);

    throw std::runtime_error("mail:: — Unknown provider: " + provider
        + " (mailgun|sendgrid|postmark|smtp)");
}

// ── LOOK module ───────────────────────────────────────────────────────────────

Module make_mail_module() {
    Module m;
    m.name = "mail";

    // mail::send($to, $subject, $text [, $html="" [, $from=""]])
    //   → {ok: bool, status: int, message: str}
    // Provider + credentials from env: MAIL_PROVIDER, MAIL_API_KEY, MAIL_FROM
    m.functions["send"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("mail::send() — expects (to, subject [, text [, html [, from]]])");

        std::string to      = args[0].to_string();
        std::string subject = args[1].to_string();
        std::string text    = (args.size() >= 3) ? args[2].to_string() : "";
        std::string html    = (args.size() >= 4) ? args[3].to_string() : "";
        std::string from    = (args.size() >= 5) ? args[4].to_string() : "";

        MailResult r = dispatch_send(to, subject, text, html, from);

        if (!r.ok) {
            Logger::instance().log(LogLevel::LOG_WARN, "mail::send",
                "Mail delivery failed [" + to + "]: " + r.message);
        }

        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        arr->push_back(Value(std::string("ok")));      arr->push_back(Value(r.ok));
        arr->push_back(Value(std::string("status")));  arr->push_back(Value(r.status));
        arr->push_back(Value(std::string("message"))); arr->push_back(Value(r.message));
        return Value(arr);
    };

    // mail::send_html($to, $subject, $html [, $from=""])
    // Convenience: HTML email, text body auto-stripped or empty.
    m.functions["send_html"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("mail::send_html() — expects (to, subject, html [, from])");

        std::string to      = args[0].to_string();
        std::string subject = args[1].to_string();
        std::string html    = args[2].to_string();
        std::string from    = (args.size() >= 4) ? args[3].to_string() : "";

        MailResult r = dispatch_send(to, subject, "", html, from);

        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        arr->push_back(Value(std::string("ok")));      arr->push_back(Value(r.ok));
        arr->push_back(Value(std::string("status")));  arr->push_back(Value(r.status));
        arr->push_back(Value(std::string("message"))); arr->push_back(Value(r.message));
        return Value(arr);
    };

    // mail::provider() → string (aktif provider adı)
    m.functions["provider"] = [](std::vector<Value>) -> Value {
        return Value(std::string(env_get("MAIL_PROVIDER", "mailgun")));
    };

    // mail::deliver_maildir($base_dir, $mailbox, $from, $data) → string (filename)
    // Delivers a raw RFC 5322 message to Maildir format.
    // Used inside smtp:: handler callbacks or job processors.
    // Example: mail::deliver_maildir("/var/mail", "inbox", "sender@example.com", $raw)
    m.functions["deliver_maildir"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 4)
            throw std::runtime_error("mail::deliver_maildir() — expects (base_dir, mailbox, from, data)");
        SmtpMessage msg;
        msg.mail_from = args[2].to_string();
        msg.data      = args[3].to_string();
        std::string fname = deliver_maildir(args[0].to_string(), args[1].to_string(), msg);
        return Value(fname);
    };

    return m;
}

} // namespace look

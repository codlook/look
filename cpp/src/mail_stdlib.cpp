// mail_stdlib.cpp — mail:: module
// Zero dependency: uses existing http_client (Schannel/OpenSSL).
// Provider selection via env MAIL_PROVIDER (mailgun|sendgrid|smtp2go|postmark).
// All providers share the same LOOK API — switch provider without code change.
#include "look/mail.h"
#include "look/http_client.h"
#include "look/logger.h"
#include <stdexcept>
#include <map>
#include <cstdlib>

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
    HttpResponse resp = http_request("POST", url, body, hdrs, opts);

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
    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
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
    HttpResponse resp = http_request("POST", "https://api.sendgrid.com/v3/mail/send",
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
    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
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
    HttpResponse resp = http_request("POST", "https://api.postmarkapp.com/email",
                                     body, hdrs, opts);
    r.status  = resp.status;
    r.ok      = (resp.status == 200);
    r.message = resp.error.empty() ? resp.body : resp.error;
    return r;
}

// ── dispatch ──────────────────────────────────────────────────────────────────
static MailResult dispatch_send(const std::string& to,
                                 const std::string& subject,
                                 const std::string& text,
                                 const std::string& html,
                                 const std::string& from_override)
{
    std::string provider = env_get("MAIL_PROVIDER", "mailgun");
    std::string api_key  = env_get("MAIL_API_KEY",  "");
    std::string from     = from_override.empty() ? env_get("MAIL_FROM", "") : from_override;

    if (api_key.empty())
        throw std::runtime_error("mail:: — MAIL_API_KEY env değişkeni eksik");
    if (from.empty())
        throw std::runtime_error("mail:: — MAIL_FROM env değişkeni eksik (veya from parametresi belirt)");

    if (provider == "mailgun") {
        std::string domain = env_get("MAIL_DOMAIN", "");
        if (domain.empty()) {
            // Extract domain from from address: "name@domain.com" → "domain.com"
            auto at = from.rfind('@');
            if (at != std::string::npos) domain = from.substr(at + 1);
        }
        if (domain.empty())
            throw std::runtime_error("mail:: — Mailgun için MAIL_DOMAIN gerekli");
        return send_mailgun(api_key, from, to, subject, text, html, domain);
    }

    if (provider == "sendgrid")
        return send_sendgrid(api_key, from, to, subject, text, html);

    if (provider == "postmark")
        return send_postmark(api_key, from, to, subject, text, html);

    throw std::runtime_error("mail:: — Bilinmeyen provider: " + provider
        + " (mailgun|sendgrid|postmark)");
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
            throw std::runtime_error("mail::send() — (to, subject [, text [, html [, from]]]) bekler");

        std::string to      = args[0].to_string();
        std::string subject = args[1].to_string();
        std::string text    = (args.size() >= 3) ? args[2].to_string() : "";
        std::string html    = (args.size() >= 4) ? args[3].to_string() : "";
        std::string from    = (args.size() >= 5) ? args[4].to_string() : "";

        MailResult r = dispatch_send(to, subject, text, html, from);

        if (!r.ok) {
            Logger::instance().log(LogLevel::LOG_WARN, "mail::send",
                "Mail gönderilemedi [" + to + "]: " + r.message);
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
            throw std::runtime_error("mail::send_html() — (to, subject, html [, from]) bekler");

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

    return m;
}

} // namespace look

#include "look/smtp_server.h"
#include "look/event_loop.h"
#include "look/logger.h"
#include "look/dns.h"
#include "look/dkim.h"
#include "look/mysql_client.h"

#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define close(x)   closesocket(x)
#  define ssize_t    int
#endif

// OpenSSL — STARTTLS support
#ifndef OPENSSL_NO_DEPRECATED_3_0
#  define OPENSSL_NO_DEPRECATED_3_0
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace look {

namespace fs = std::filesystem;

// ── DoS limits (env-configurable) ────────────────────────────────────────────

static int smtp_max_conn() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_CONN");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 1000;
    }();
    return v;
}
static size_t smtp_max_msg() {
    static size_t v = []() -> size_t {
        const char* e = std::getenv("LOOK_SMTP_MAX_MSG_SIZE");
        if (e && *e) { long x = std::atol(e); if (x > 0) return (size_t)x; }
        return 25ul * 1024 * 1024; // 25 MB
    }();
    return v;
}
static int smtp_max_rcpt() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_RCPT");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 100;
    }();
    return v;
}
static int smtp_max_errors() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_ERRORS");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 5;
    }();
    return v;
}
static std::string smtp_dkim_domain() {
    static std::string v = []() -> std::string {
        const char* e = std::getenv("LOOK_SMTP_DKIM_DOMAIN");
        return (e && *e) ? e : "";
    }();
    return v;
}
static std::string smtp_dkim_selector() {
    static std::string v = []() -> std::string {
        const char* e = std::getenv("LOOK_SMTP_DKIM_SELECTOR");
        return (e && *e) ? e : "mail";
    }();
    return v;
}
static std::string smtp_dkim_key() {
    // LOOK_SMTP_DKIM_KEY_FILE: path to RSA private key PEM file
    // Read each time — key file may be mounted after server starts
    const char* e = std::getenv("LOOK_SMTP_DKIM_KEY_FILE");
    if (!e || !*e) return "";
    std::ifstream f(e);
    if (!f) return "";
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string smtp_banner() {
    static std::string v = []() -> std::string {
        const char* e = std::getenv("LOOK_SMTP_BANNER");
        return (e && *e) ? e : "localhost";
    }();
    return v;
}
static int smtp_max_conns_per_ip() {
    static int v = []() {
        const char* e = std::getenv("LOOK_SMTP_MAX_CONNS_IP");
        if (e && *e) { int x = std::atoi(e); if (x > 0) return x; }
        return 10;
    }();
    return v;
}

// ── Per-IP connection tracking ────────────────────────────────────────────────
static std::mutex                            g_ip_mutex;
static std::unordered_map<std::string, int>  g_ip_conns;

static bool ip_acquire(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_ip_mutex);
    int& n = g_ip_conns[ip];
    if (n >= smtp_max_conns_per_ip()) return false;
    ++n; return true;
}
static void ip_release(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_ip_mutex);
    auto it = g_ip_conns.find(ip);
    if (it != g_ip_conns.end() && --it->second <= 0) g_ip_conns.erase(it);
}

// ── AUTH PLAIN validation ─────────────────────────────────────────────────────
// RFC 4616 base64-decode "\0authcid\0passwd"
static std::string smtp_b64_decode(const std::string& s) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        const char* p = std::strchr(tbl, c);
        if (!p) continue;
        val = (val << 6) | (int)(p - tbl);
        bits += 6;
        if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

// PBKDF2-HMAC-SHA256 — pbkdf2$sha256$iterations$salt_b64$hash_b64 formatini dogrular
// auth::hash() ile ayni format, constant-time karsilastirma.
static std::vector<uint8_t> smtp_hmac_sha256(const std::vector<uint8_t>& key,
                                              const std::vector<uint8_t>& data) {
    // Inner/outer hash — HMAC-SHA256 (portable, no dependency)
    auto sha256 = [](const std::vector<uint8_t>& in) -> std::vector<uint8_t> {
        // SHA-256 via OpenSSL EVP (already linked for TLS)
        std::vector<uint8_t> out(32);
        unsigned int len = 32;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, in.data(), in.size());
        EVP_DigestFinal_ex(ctx, out.data(), &len);
        EVP_MD_CTX_free(ctx);
        return out;
    };
    const size_t BS = 64;
    std::vector<uint8_t> k = key;
    if (k.size() > BS) k = sha256(k);
    k.resize(BS, 0);
    std::vector<uint8_t> inner(BS + data.size()), outer(BS + 32);
    for (size_t i = 0; i < BS; i++) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
    std::copy(data.begin(), data.end(), inner.begin() + BS);
    auto h1 = sha256(inner);
    std::copy(h1.begin(), h1.end(), outer.begin() + BS);
    return sha256(outer);
}

static std::vector<uint8_t> smtp_pbkdf2_sha256(const std::string& password,
                                                const std::vector<uint8_t>& salt,
                                                uint32_t iterations, uint32_t dk_len) {
    std::vector<uint8_t> pwd(password.begin(), password.end());
    std::vector<uint8_t> dk;
    uint32_t blocks = (dk_len + 31) / 32;
    for (uint32_t block = 1; block <= blocks; block++) {
        std::vector<uint8_t> sb = salt;
        sb.push_back((block>>24)&0xFF); sb.push_back((block>>16)&0xFF);
        sb.push_back((block>>8)&0xFF);  sb.push_back(block&0xFF);
        auto u = smtp_hmac_sha256(pwd, sb);
        auto f = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = smtp_hmac_sha256(pwd, u);
            for (size_t j = 0; j < f.size(); j++) f[j] ^= u[j];
        }
        dk.insert(dk.end(), f.begin(), f.end());
    }
    dk.resize(dk_len);
    return dk;
}

static std::vector<uint8_t> smtp_b64_decode_bytes(const std::string& s) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        const char* p = std::strchr(tbl, c);
        if (!p) continue;
        val = (val << 6) | (int)(p - tbl);
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// pbkdf2$sha256$<iter>$<salt_b64>$<hash_b64> formatindaki hash'i dogrular
static bool smtp_pbkdf2_verify(const std::string& password, const std::string& stored_hash) {
    std::vector<std::string> parts;
    std::istringstream ss(stored_hash);
    std::string tok;
    while (std::getline(ss, tok, '$')) parts.push_back(tok);
    if (parts.size() < 5 || parts[0] != "pbkdf2") return false;
    uint32_t iter = 0;
    try { iter = (uint32_t)std::stoi(parts[2]); } catch (...) { return false; }  // bozuk hash → doğrulama başarısız
    if (iter == 0) return false;
    auto     salt   = smtp_b64_decode_bytes(parts[3]);
    auto     stored = smtp_b64_decode_bytes(parts[4]);
    auto     derived = smtp_pbkdf2_sha256(password, salt, iter, 32);
    if (derived.size() != stored.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < derived.size(); i++) diff |= derived[i] ^ stored[i];
    return diff == 0;
}

// DSN'den MySQL baglantisi olustur — smtp_validate_auth icin
static std::shared_ptr<MySQLClient> smtp_make_db_conn(const std::string& dsn) {
    std::string user, pass, host, db;
    int port = 3306;
    // mysql://user:pass@host:port/db
    size_t se = dsn.find("://");
    if (se == std::string::npos) throw std::runtime_error("invalid DSN");
    std::string rest = dsn.substr(se + 3);
    size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        std::string ui = rest.substr(0, at); rest = rest.substr(at + 1);
        size_t c = ui.find(':');
        if (c != std::string::npos) { user = ui.substr(0, c); pass = ui.substr(c + 1); }
        else user = ui;
    }
    size_t sl = rest.find('/');
    std::string hp = (sl != std::string::npos) ? rest.substr(0, sl) : rest;
    db = (sl != std::string::npos) ? rest.substr(sl + 1) : "";
    size_t c = hp.find(':');
    if (c != std::string::npos) {
        host = hp.substr(0, c);
        try { port = std::stoi(hp.substr(c + 1)); } catch (...) { /* varsayılan port korunur */ }
    }
    else host = hp;

    DbConfig cfg;
    cfg.host = host; cfg.port = port;
    cfg.user = user; cfg.password = pass; cfg.database = db;
    auto conn = std::make_shared<MySQLClient>();
    conn->connect(cfg);
    return conn;
}

static bool smtp_validate_auth(const std::string& b64) {
    const char* disabled = std::getenv("LOOK_SMTP_AUTH_DISABLED");
    if (disabled && std::string(disabled) == "true") return false;

    std::string plain = smtp_b64_decode(b64);
    // Format: [authzid NUL] authcid NUL passwd
    std::vector<std::string> parts;
    std::string part;
    for (char c : plain) {
        if (c == '\0') { parts.push_back(part); part.clear(); }
        else part += c;
    }
    parts.push_back(part);
    if (parts.size() < 2) return false;
    std::string passwd = parts.back();
    std::string username = (parts.size() >= 3) ? parts[parts.size()-2] : parts[0];
    if (passwd.empty()) return false;

    // DB dogrulama: LOOK_SMTP_USER_DB set edilmisse DB'den sorgula
    const char* db_dsn = std::getenv("LOOK_SMTP_USER_DB");
    if (db_dsn && *db_dsn) {
        try {
            auto conn = smtp_make_db_conn(db_dsn);
            // Hazir sorgu: email ve active kontrolu
            auto rows = conn->execute(
                "SELECT password FROM mail_users WHERE email=? AND active=1",
                {DbParam::from_text(username)});
            if (rows.empty()) return false;
            // DbRow = vector<pair<string, DbValue>>, DbValue.str = string deger
            for (auto& col : rows[0]) {
                if (col.first == "password")
                    return smtp_pbkdf2_verify(passwd, col.second.str);
            }
            return false;
        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                std::string("auth DB error: ") + e.what());
            return false; // DB hatasi → reddet (guvenli taraf)
        }
    }

    // Fallback: tek token (gelistirme / basit deployment)
    const char* token = std::getenv("LOOK_SMTP_AUTH_TOKEN");
    if (token && *token) return passwd == std::string(token);
    return false;
}

// ── Ortak mail kimlik doğrulama (IMAP LOGIN + gelecekteki servisler) ─────────
bool mail_user_auth(const std::string& user, const std::string& pass,
                    std::string& maildir_out) {
    maildir_out.clear();
    if (user.empty() || pass.empty()) return false;
    // GÜVENLİK: kullanıcı adı Maildir yol bileşeni olacak — traversal/kontrol
    // karakterlerini baştan reddet (doğrulanmış olsa bile defense-in-depth).
    for (unsigned char c : user)
        if (c == '/' || c == '\\' || c == '\0' || c < 0x20) return false;
    if (user.find("..") != std::string::npos) return false;

    bool ok = false;
    const char* db_dsn = std::getenv("LOOK_MAIL_USER_DB");
    if (!db_dsn || !*db_dsn) db_dsn = std::getenv("LOOK_SMTP_USER_DB");
    if (db_dsn && *db_dsn) {
        try {
            auto conn = smtp_make_db_conn(db_dsn);
            auto rows = conn->execute(
                "SELECT password FROM mail_users WHERE email=? AND active=1",
                {DbParam::from_text(user)});
            if (!rows.empty())
                for (auto& col : rows[0])
                    if (col.first == "password") { ok = smtp_pbkdf2_verify(pass, col.second.str); break; }
        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::LOG_ERROR, "imap",
                std::string("auth DB error: ") + e.what());
            return false;   // DB hatası → reddet (güvenli taraf)
        }
    } else {
        // Fallback: tek kullanıcı (dev / basit deployment) — sabit-zamanlı karşılaştırma
        const char* du = std::getenv("LOOK_MAIL_USER");
        const char* dp = std::getenv("LOOK_MAIL_PASS");
        if (du && dp && *du && *dp) {
            std::string eu(du), ep(dp);
            uint8_t diff = (uint8_t)(user.size() ^ eu.size()) | (uint8_t)(pass.size() ^ ep.size());
            for (size_t i = 0; i < user.size(); i++) diff |= (uint8_t)(user[i] ^ eu[i % eu.size()]);
            for (size_t i = 0; i < pass.size(); i++) diff |= (uint8_t)(pass[i] ^ ep[i % ep.size()]);
            ok = (diff == 0) && (user == eu) && (pass == ep);
        }
    }
    if (!ok) return false;

    const char* base = std::getenv("LOOK_MAIL_DIR");
    if (!base || !*base) base = "/var/mail/look";
    // INBOX = SMTP teslimatıyla aynı yol: <base>/<user>/inbox
    maildir_out = (fs::path(base) / user / "inbox").string();
    return true;
}

// ── Local domain check for open relay prevention ─────────────────────────────
static bool is_local_domain(const std::string& addr_domain) {
    std::string cfg = []() -> std::string {
        const char* e = std::getenv("LOOK_SMTP_LOCAL_DOMAINS");
        if (e && *e) return e;
        e = std::getenv("LOOK_MAIL_DOMAIN");
        return (e && *e) ? e : "";
    }();
    if (cfg.empty()) return false; // no domains configured → reject all
    std::istringstream ss(cfg);
    std::string d;
    while (std::getline(ss, d, ',')) {
        while (!d.empty() && d.front() == ' ') d = d.substr(1);
        while (!d.empty() && d.back()  == ' ') d.pop_back();
        if (d == addr_domain) return true;
    }
    return false;
}

// ── SMTP session state machine ────────────────────────────────────────────────

enum class SmtpState {
    GREETING,        // connection opened, send 220
    EHLO,            // waiting for EHLO/HELO
    READY,           // post-EHLO, waiting for AUTH/MAIL/QUIT/RSET/STARTTLS
    STARTTLS_WAIT,   // sent 220 Go ahead, waiting for TLS ClientHello
    MAIL_FROM,       // received MAIL FROM, waiting for RCPT TO
    RCPT_TO,         // received ≥1 RCPT TO, waiting for more or DATA
    DATA_INIT,       // received DATA, sending 354
    DATA_BODY,       // accumulating message body until lone "."
    QUIT,            // QUIT received, closing
};

struct SmtpSession {
    int         fd       = -1;
    SmtpState   state    = SmtpState::GREETING;
    std::string buf;           // raw read buffer (may span multiple reads)
    SmtpMessage msg;           // in-progress message
    std::string ehlo_domain;   // client EHLO domain
    int         error_count = 0;
    bool        submission  = false; // connected on :587
    bool        auth_ok     = false; // AUTH PLAIN succeeded

    // TLS — set after STARTTLS handshake completes
    SSL*        ssl      = nullptr;
    bool        tls_active = false;

    ~SmtpSession() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
    }

    // Reset envelope for RSET / after successful delivery
    void reset_envelope() {
        msg.mail_from.clear();
        msg.rcpt_to.clear();
        msg.data.clear();
        state = SmtpState::READY;
    }
};

// ── Address extraction ─────────────────────────────────────────────────────────
// Extract <addr> from "MAIL FROM:<addr>" or "RCPT TO:<addr>".
static std::string extract_addr(const std::string& line) {
    size_t lt = line.find('<');
    size_t gt = line.find('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt)
        return line.substr(lt + 1, gt - lt - 1);
    // Bare address without angle brackets
    size_t sp = line.find(' ');
    if (sp != std::string::npos) return line.substr(sp + 1);
    return "";
}

// ── SMTP command tokenizer ────────────────────────────────────────────────────
static std::string smtp_verb(const std::string& line) {
    std::string v;
    for (char c : line) {
        if (c == ' ' || c == '\r' || c == '\n') break;
        v += (char)std::toupper((unsigned char)c);
    }
    return v;
}

// ── SPF check (RFC 7208) ──────────────────────────────────────────────────────

// Match IPv4 CIDR: ip4:A.B.C.D/N
static bool spf_match_ip4(const std::string& mechanism,
                           const std::string& remote_ip) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    std::string cidr = mechanism.substr(4); // strip "ip4:"
    size_t slash = cidr.find('/');
    std::string net_str  = (slash != std::string::npos) ? cidr.substr(0, slash) : cidr;
    int         prefix   = (slash != std::string::npos) ? std::atoi(cidr.c_str() + slash + 1) : 32;
    if (prefix < 0 || prefix > 32) return false;

    // Parse network address
    struct in_addr net_addr{}, rem_addr{};
#if defined(_WIN32)
    if (InetPtonA(AF_INET, net_str.c_str(), &net_addr) != 1) return false;
    if (InetPtonA(AF_INET, remote_ip.c_str(), &rem_addr) != 1) return false;
#else
    if (inet_pton(AF_INET, net_str.c_str(), &net_addr) != 1) return false;
    if (inet_pton(AF_INET, remote_ip.c_str(), &rem_addr) != 1) return false;
#endif
    if (prefix == 0) return true;
    uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : (~0u << (32 - prefix));
    uint32_t n = ntohl(net_addr.s_addr);
    uint32_t r = ntohl(rem_addr.s_addr);
    return (n & mask) == (r & mask);
#else
    (void)mechanism; (void)remote_ip;
    return false;
#endif
}

// Evaluate one level of SPF record for `remote_ip`.
// Returns SpfResult — does NOT follow include: recursion beyond one level.
static SpfResult spf_evaluate(const std::string& record,
                               const std::string& remote_ip,
                               int depth = 0) {
    if (depth > 9) return SpfResult::NEUTRAL; // RFC 7208 §4.6.4: max 10 DNS lookups

    std::istringstream ss(record);
    std::string token;
    while (ss >> token) {
        if (token == "v=spf1") continue;

        // Qualifier: +pass -fail ~softfail ?neutral (default +)
        char qual = '+';
        std::string mech = token;
        if (!token.empty() && (token[0]=='+' || token[0]=='-' ||
                                token[0]=='~' || token[0]=='?')) {
            qual = token[0];
            mech = token.substr(1);
        }
        auto result_for = [&]() -> SpfResult {
            switch (qual) {
                case '+': return SpfResult::PASS;
                case '-': return SpfResult::FAIL;
                case '~': return SpfResult::SOFTFAIL;
                default:  return SpfResult::NEUTRAL;
            }
        };

        // Lowercase mechanism name for comparison
        std::string ml = mech;
        for (char& c : ml) c = (char)std::tolower((unsigned char)c);

        if (ml == "all")  return result_for();
        if (ml.substr(0, 4) == "ip4:") {
            if (spf_match_ip4(ml, remote_ip)) return result_for();
            continue;
        }
        if (ml.substr(0, 8) == "include:") {
            std::string inc_domain = mech.substr(8);
            try {
                auto txts = dns_txt_lookup(inc_domain);
                for (auto& t : txts) {
                    if (t.find("v=spf1") != std::string::npos) {
                        SpfResult r = spf_evaluate(t, remote_ip, depth + 1);
                        if (r == SpfResult::PASS) return SpfResult::PASS;
                        if (r == SpfResult::FAIL) return SpfResult::FAIL;
                        break;
                    }
                }
            } catch (...) {}
            continue;
        }
        // redirect= modifier
        if (ml.substr(0, 9) == "redirect=") {
            std::string redir = mech.substr(9);
            try {
                auto txts = dns_txt_lookup(redir);
                for (auto& t : txts) {
                    if (t.find("v=spf1") != std::string::npos)
                        return spf_evaluate(t, remote_ip, depth + 1);
                }
            } catch (...) {}
            return SpfResult::NEUTRAL;
        }
        // ip6: — skip (IPv6 not yet in scope)
        // mx:, a: — require additional DNS lookups; skip for minimal impl
    }
    return SpfResult::NEUTRAL;
}

static SpfResult spf_check(const std::string& mail_from,
                            const std::string& remote_ip) {
    // Extract domain from MAIL FROM address
    size_t at = mail_from.rfind('@');
    if (at == std::string::npos || at + 1 >= mail_from.size())
        return SpfResult::NONE;
    std::string domain = mail_from.substr(at + 1);
    // Strip trailing '>' if present
    if (!domain.empty() && domain.back() == '>') domain.pop_back();

    std::vector<std::string> txts;
    try { txts = dns_txt_lookup(domain); }
    catch (...) { return SpfResult::NONE; }

    for (auto& t : txts) {
        if (t.find("v=spf1") != std::string::npos)
            return spf_evaluate(t, remote_ip);
    }
    return SpfResult::NONE; // no SPF record
}

// ── Worker thread pool ────────────────────────────────────────────────────────

struct WorkItem {
    SmtpMessage msg;
    int         fd;
    SmtpHandler handler;
    EventLoop*  loop;
    std::shared_ptr<SmtpSession> session;
};

struct WorkerPool {
    std::vector<std::thread>      threads;
    std::queue<WorkItem>          queue;
    std::mutex                    mtx;
    std::condition_variable       cv;
    std::atomic<bool>             stop{false};

    void start(int n) {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([this]{ worker_loop(); });
    }
    void shutdown() {
        stop = true;
        cv.notify_all();
        for (auto& t : threads) if (t.joinable()) t.join();
    }
    void enqueue(WorkItem item) {
        std::lock_guard<std::mutex> lk(mtx);
        queue.push(std::move(item));
        cv.notify_one();
    }

    // For non-delivery tasks (e.g. STARTTLS handshake) — direct lambda
    using RawFn = std::function<void()>;
    std::queue<RawFn> raw_queue;
    void enqueue_raw(RawFn fn) {
        std::lock_guard<std::mutex> lk(mtx);
        raw_queue.push(std::move(fn));
        cv.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            // Prefer raw tasks (e.g. STARTTLS) over delivery items
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this]{ return stop || !queue.empty() || !raw_queue.empty(); });
                if (stop && queue.empty() && raw_queue.empty()) return;
                if (!raw_queue.empty()) {
                    RawFn fn = std::move(raw_queue.front());
                    raw_queue.pop();
                    lk.unlock();
                    try { fn(); } catch (...) {}
                    continue;
                }
            }
            WorkItem item;
            {
                std::unique_lock<std::mutex> lk(mtx);
                if (queue.empty()) continue;
                item = std::move(queue.front());
                queue.pop();
            }
            // SPF — blocking DNS is safe here (worker thread, not EventLoop)
            item.msg.spf = spf_check(item.msg.mail_from, item.msg.remote_ip);
            if (item.msg.spf == SpfResult::FAIL) {
                Logger::instance().log(LogLevel::LOG_WARN, "smtp",
                    "SPF FAIL for " + item.msg.mail_from + " from " + item.msg.remote_ip);
            }

            // DKIM sign outbound: authenticated submission (:587) with key configured.
            // tls_active is checked only when ssl_ctx exists; without a cert,
            // auth still works so we still sign.
            if ((item.msg.tls || item.msg.authenticated)
                    && !smtp_dkim_domain().empty() && !smtp_dkim_key().empty()
                    && !item.msg.data.empty()) {
                try {
                    // Parse headers from msg.data (split at first blank line)
                    size_t hdr_end = item.msg.data.find("\r\n\r\n");
                    std::string hdr_block = (hdr_end != std::string::npos)
                        ? item.msg.data.substr(0, hdr_end) : item.msg.data;
                    std::vector<DkimHeader> hdrs;
                    std::istringstream hss(hdr_block);
                    std::string hline;
                    while (std::getline(hss, hline)) {
                        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
                        auto colon = hline.find(':');
                        if (colon == std::string::npos) continue;
                        std::string hname = hline.substr(0, colon);
                        std::string hval  = hline.substr(colon + 1);
                        hval.erase(0, hval.find_first_not_of(" \t"));
                        hdrs.push_back({hname, hval});
                    }
                    std::string body = (hdr_end != std::string::npos)
                        ? item.msg.data.substr(hdr_end + 4) : "";
                    std::string sig = dkim_sign(hdrs, body, smtp_dkim_domain(),
                                                smtp_dkim_selector(), smtp_dkim_key());
                    item.msg.data = sig + "\r\n" + item.msg.data;
                } catch (const std::exception& ex) {
                    Logger::instance().log(LogLevel::LOG_WARN, "smtp",
                        std::string("DKIM sign failed: ") + ex.what());
                }
            }

            // DKIM verify inbound — also safe on worker thread
            if (!item.msg.data.empty()) {
                try { item.msg.dkim_ok = dkim_verify(item.msg.data); }
                catch (...) { item.msg.dkim_ok = false; }
            }

            bool ok = false;
            try { ok = item.handler(item.msg); }
            catch (const std::exception& e) {
                Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                    std::string("handler exception: ") + e.what());
            }
            // Reply on event loop thread
            std::string reply = ok
                ? "250 2.6.0 Message accepted\r\n"
                : "550 5.7.1 Message rejected by policy\r\n";
            item.loop->post([loop = item.loop, fd = item.fd, reply, session = item.session]() mutable {
                loop->async_write(fd, reply, [loop, fd, session](bool) {
                    session->reset_envelope();
                });
            });
        }
    }
};

// ── SmtpServer::Impl ──────────────────────────────────────────────────────────

// ── TLS server context (shared, created once) ─────────────────────────────────
static SSL_CTX* make_smtp_ssl_ctx() {
    // Cert/key from env — LOOK_SMTP_CERT and LOOK_SMTP_KEY
    const char* cert = std::getenv("LOOK_SMTP_CERT");
    const char* key  = std::getenv("LOOK_SMTP_KEY");
    if (!cert || !key) return nullptr; // TLS not configured

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION); // TLS 1.2 minimum
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                              SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx,  key,  SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
            "STARTTLS: sertifika/anahtar yüklenemedi");
        return nullptr;
    }
    return ctx;
}

struct SmtpServer::Impl {
    std::unique_ptr<EventLoop> loop;
    WorkerPool                 pool;
    SmtpHandler                handler;
    std::atomic<int>           conn_count{0};
    SSL_CTX*                   ssl_ctx = nullptr; // null = TLS not configured

    // Per-connection session map (fd → session), guarded by loop thread
    std::unordered_map<int, std::shared_ptr<SmtpSession>> sessions;

    // ── Server socket helpers ────────────────────────────────────────────────
    static int make_server_fd(int port) {
#if defined(_WIN32)
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
#else
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
#endif
        if (fd < 0) return -1;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        // Dual-stack: accept both IPv4 and IPv6
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no));

        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons((uint16_t)port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd); return -1;
        }
        if (::listen(fd, 128) < 0) { close(fd); return -1; }
        return fd;
    }

    // ── Session I/O ──────────────────────────────────────────────────────────
    void on_accept(int fd, bool submission) {
        // Extract remote IP via getpeername
        std::string remote_ip = "unknown";
        {
            struct sockaddr_storage sa{};
            socklen_t salen = sizeof(sa);
            if (::getpeername(fd, (struct sockaddr*)&sa, &salen) == 0) {
                char buf[INET6_ADDRSTRLEN] = {};
                if (sa.ss_family == AF_INET6) {
                    auto* s6 = (struct sockaddr_in6*)&sa;
                    if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr))
                        inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], buf, sizeof(buf));
                    else
                        inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
                } else {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)&sa)->sin_addr, buf, sizeof(buf));
                }
                remote_ip = buf;
            }
        }

        // Per-IP connection limit
        if (!ip_acquire(remote_ip)) {
            const char* msg = "421 4.3.2 Too many connections from your IP\r\n";
#if defined(_WIN32)
            send(fd, msg, (int)strlen(msg), 0);
#else
            ::send(fd, msg, strlen(msg), MSG_NOSIGNAL);
#endif
            close(fd); return;
        }

        if (conn_count.load() >= smtp_max_conn()) {
            ip_release(remote_ip);
            const char* busy = "421 4.3.2 Service temporarily unavailable\r\n";
#if defined(_WIN32)
            send(fd, busy, (int)strlen(busy), 0);
#else
            ::send(fd, busy, strlen(busy), MSG_NOSIGNAL);
#endif
            close(fd); return;
        }
        conn_count.fetch_add(1);

        auto sess = std::make_shared<SmtpSession>();
        sess->fd            = fd;
        sess->state         = SmtpState::GREETING;
        sess->submission    = submission;
        sess->msg.remote_ip = remote_ip;
        sessions[fd]        = sess;

        std::string banner = "220 " + smtp_banner() + " ESMTP LOOK\r\n";
        loop->async_write(fd, banner, [this, fd, sess](bool ok) {
            if (!ok) { close_session(fd); return; }
            sess->state = SmtpState::EHLO;
            loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
                on_data(fd, sess, data, len);
            });
        });
    }

    void close_session(int fd) {
        auto it = sessions.find(fd);
        if (it != sessions.end()) {
            ip_release(it->second->msg.remote_ip);
            sessions.erase(it);
        }
        loop->close_fd(fd);
        conn_count.fetch_sub(1);
    }

    void send_reply(int fd, std::shared_ptr<SmtpSession> sess,
                    const std::string& reply,
                    std::function<void()> then = nullptr) {
        loop->async_write(fd, reply, [this, fd, sess, then](bool ok) {
            if (!ok) { close_session(fd); return; }
            if (then) then();
            else {
                // Keep reading
                loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
                    on_data(fd, sess, data, len);
                });
            }
        });
    }

    void on_data(int fd, std::shared_ptr<SmtpSession> sess,
                 const char* data, size_t len) {
        if (len == 0) { close_session(fd); return; }
        sess->buf.append(data, len);

        // Line-by-line processing from accumulated buffer.
        // Handles ".\r\n" split across chunks correctly: we only extract a line
        // when '\n' is present in buf — partial lines stay in buf until next read.
        while (true) {
            size_t nl = sess->buf.find('\n');
            if (nl == std::string::npos) break;

            std::string line = sess->buf.substr(0, nl);
            sess->buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            bool keep_reading = dispatch_line(fd, sess, line);
            if (!keep_reading) return; // session handed off or closed
        }

        // Need more data
        loop->async_read(fd, [this, fd, sess](const char* data, size_t len) {
            on_data(fd, sess, data, len);
        });
    }

    // Returns false when reading is handed off to async_write or session is closed
    bool dispatch_line(int fd, std::shared_ptr<SmtpSession> sess,
                       const std::string& line) {
        if (sess->state == SmtpState::DATA_BODY) {
            return handle_data_body(fd, sess, line);
        }

        std::string verb = smtp_verb(line);

        // QUIT — always valid
        if (verb == "QUIT") {
            send_reply(fd, sess, "221 2.0.0 Bye\r\n", [this, fd]{ close_session(fd); });
            return false;
        }

        // RSET — always valid post-EHLO
        if (verb == "RSET") {
            sess->reset_envelope();
            send_reply(fd, sess, "250 2.0.0 Reset\r\n");
            return false;
        }

        // NOOP
        if (verb == "NOOP") {
            send_reply(fd, sess, "250 2.0.0 OK\r\n");
            return false;
        }

        switch (sess->state) {
            case SmtpState::EHLO:
                return handle_ehlo(fd, sess, verb, line);
            case SmtpState::READY:
                return handle_ready(fd, sess, verb, line);
            case SmtpState::MAIL_FROM:
                return handle_mail_from_state(fd, sess, verb, line);
            case SmtpState::RCPT_TO:
                return handle_rcpt_to_state(fd, sess, verb, line);
            case SmtpState::DATA_INIT:
                return handle_data_init(fd, sess, verb, line);
            default:
                return error_reply(fd, sess, "503 5.5.1 Bad sequence\r\n");
        }
    }

    bool handle_ehlo(int fd, std::shared_ptr<SmtpSession> sess,
                     const std::string& verb, const std::string& line) {
        if (verb != "EHLO" && verb != "HELO") {
            return error_reply(fd, sess, "503 5.5.1 Send EHLO first\r\n");
        }
        size_t sp = line.find(' ');
        sess->ehlo_domain = (sp != std::string::npos) ? line.substr(sp + 1) : "unknown";

        std::string r = "250-" + smtp_banner() + "\r\n"
                        "250-SIZE " + std::to_string(smtp_max_msg()) + "\r\n"
                        "250-8BITMIME\r\n"
                        "250-PIPELINING\r\n"
                        "250-SMTPUTF8\r\n";
        if (ssl_ctx && !sess->tls_active)
            r += "250-STARTTLS\r\n";
        if (sess->submission)
            r += "250-AUTH PLAIN LOGIN\r\n";
        r += "250 OK\r\n";
        sess->state = SmtpState::READY;
        send_reply(fd, sess, r);
        return false;
    }

    bool handle_ready(int fd, std::shared_ptr<SmtpSession> sess,
                      const std::string& verb, const std::string& line) {
        if (verb == "STARTTLS") {
            if (sess->tls_active) {
                return error_reply(fd, sess, "503 5.5.1 TLS already active\r\n");
            }
            if (!ssl_ctx) {
                return error_reply(fd, sess, "454 4.7.0 TLS not configured\r\n");
            }
            send_reply(fd, sess, "220 2.0.0 Ready to start TLS\r\n",
                [this, fd, sess]() { do_starttls(fd, sess); });
            return false;
        }
        // AUTH PLAIN [initial-response]
        if (verb == "AUTH") {
            if (!sess->submission) {
                return error_reply(fd, sess, "503 5.5.1 AUTH not permitted on port 25\r\n");
            }
            if (sess->auth_ok) {
                return error_reply(fd, sess, "503 5.5.1 Already authenticated\r\n");
            }
            if (!sess->tls_active && ssl_ctx) {
                return error_reply(fd, sess, "530 5.7.0 Must issue STARTTLS first\r\n");
            }
            // Extract mechanism and optional initial response
            // Line format: "AUTH PLAIN [base64]" or "AUTH PLAIN"
            size_t mech_start = line.find(' ');
            std::string rest = (mech_start != std::string::npos)
                ? line.substr(mech_start + 1) : "";
            // rest = "PLAIN" or "PLAIN <b64>" or "LOGIN ..."
            size_t sp2 = rest.find(' ');
            std::string mech    = (sp2 != std::string::npos) ? rest.substr(0, sp2) : rest;
            std::string initial = (sp2 != std::string::npos) ? rest.substr(sp2 + 1) : "";
            for (char& c : mech) c = (char)std::toupper((unsigned char)c);
            if (mech != "PLAIN" && mech != "LOGIN") {
                return error_reply(fd, sess, "504 5.7.4 AUTH mechanism not supported\r\n");
            }
            if (smtp_validate_auth(initial)) {
                sess->auth_ok    = true;
                sess->msg.authenticated = true;
                send_reply(fd, sess, "235 2.7.0 Authentication successful\r\n");
            } else {
                error_reply(fd, sess, "535 5.7.8 Authentication credentials invalid\r\n");
            }
            return false;
        }
        if (verb == "MAIL") {
            // :587 submission requires TLS before MAIL FROM (RFC 8314)
            if (sess->submission && !sess->tls_active && ssl_ctx) {
                return error_reply(fd, sess, "530 5.7.0 Must issue STARTTLS first\r\n");
            }
            // :587 requires AUTH before MAIL FROM
            if (sess->submission && !sess->auth_ok) {
                return error_reply(fd, sess, "530 5.7.0 Authentication required\r\n");
            }
            std::string addr = extract_addr(line);
            if (addr.empty()) {
                return error_reply(fd, sess, "501 5.1.7 Bad sender address\r\n");
            }
            sess->msg.mail_from = addr;
            sess->msg.remote_ip = sess->msg.remote_ip; // already set on accept
            sess->state = SmtpState::MAIL_FROM;
            send_reply(fd, sess, "250 2.1.0 OK\r\n");
            return false;
        }
        if (verb == "EHLO" || verb == "HELO") {
            return handle_ehlo(fd, sess, verb, line);
        }
        return error_reply(fd, sess, "503 5.5.1 Need MAIL command\r\n");
    }

    void do_starttls(int fd, std::shared_ptr<SmtpSession> sess) {
        // Detach fd from EventLoop BEFORE handing it to the worker thread.
        // This prevents epoll/IOCP from delivering events on the same fd
        // while SSL_accept() is reading from it on the worker thread.
        // add_client() re-registers the fd after the handshake completes.
        loop->detach_fd(fd);

        pool.enqueue_raw([this, fd, sess]() {
            // fd is now owned exclusively by this worker thread — set blocking
#if defined(__linux__) || defined(__APPLE__)
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
            SSL* ssl = SSL_new(ssl_ctx);
            if (!ssl) {
                loop->post([this, fd]{ close_session(fd); });
                return;
            }
            SSL_set_fd(ssl, fd);
            int r = SSL_accept(ssl);
            if (r != 1) {
                unsigned long e = ERR_get_error();
                char buf[256]; ERR_error_string_n(e, buf, sizeof(buf));
                Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                    std::string("STARTTLS handshake failed: ") + buf);
                SSL_free(ssl);
                loop->post([this, fd]{ close_session(fd); });
                return;
            }
            // Handshake complete — restore non-blocking, update session
            sess->ssl        = ssl;
            sess->tls_active = true;
            sess->msg.tls    = true;
            sess->state      = SmtpState::EHLO; // client must re-EHLO after STARTTLS
            // Re-wrap fd with TLS-aware reads via BIO
            // For simplicity: re-register with EventLoop using SSL_read wrapper
            loop->post([this, fd, sess]() {
                // Re-register fd for reading — session now uses SSL_read path
                loop->add_client(fd, [this, fd, sess](const char* data, size_t len) {
                    on_data_tls(fd, sess, data, len);
                });
            });
        });
    }

    // TLS-aware read path — called after STARTTLS handshake
    // EventLoop still delivers raw bytes, but we pass through SSL_read
    void on_data_tls(int fd, std::shared_ptr<SmtpSession> sess,
                     const char* /*raw*/, size_t /*raw_len*/) {
        // With SSL_set_fd the BIO wraps the fd — SSL_read handles decryption
        char buf[4096];
        while (true) {
            int n = SSL_read(sess->ssl, buf, sizeof(buf));
            if (n <= 0) {
                int err = SSL_get_error(sess->ssl, n);
                if (err == SSL_ERROR_WANT_READ) break; // need more data
                close_session(fd); return;
            }
            on_data(fd, sess, buf, (size_t)n);
        }
    }

    // Validate recipient: open relay prevention + rcpt limit
    bool validate_rcpt(int fd, std::shared_ptr<SmtpSession> sess,
                       const std::string& addr) {
        if (addr.empty()) {
            error_reply(fd, sess, "501 5.1.3 Bad recipient address\r\n");
            return false;
        }
        if ((int)sess->msg.rcpt_to.size() >= smtp_max_rcpt()) {
            error_reply(fd, sess, "452 4.5.3 Too many recipients\r\n");
            return false;
        }
        // Authenticated submission may relay outbound freely.
        // Unauthenticated :25 MTA connections may only deliver to local domains.
        if (!sess->auth_ok) {
            size_t at = addr.rfind('@');
            std::string domain = (at != std::string::npos) ? addr.substr(at + 1) : "";
            if (!is_local_domain(domain)) {
                error_reply(fd, sess, "550 5.7.1 Relay access denied — "
                    "set LOOK_SMTP_LOCAL_DOMAINS\r\n");
                return false;
            }
        }
        return true;
    }

    bool handle_mail_from_state(int fd, std::shared_ptr<SmtpSession> sess,
                                const std::string& verb, const std::string& line) {
        if (verb != "RCPT") {
            return error_reply(fd, sess, "503 5.5.1 Need RCPT TO\r\n");
        }
        std::string addr = extract_addr(line);
        if (!validate_rcpt(fd, sess, addr)) return false;
        sess->msg.rcpt_to.push_back(addr);
        sess->state = SmtpState::RCPT_TO;
        send_reply(fd, sess, "250 2.1.5 OK\r\n");
        return false;
    }

    bool handle_rcpt_to_state(int fd, std::shared_ptr<SmtpSession> sess,
                               const std::string& verb, const std::string& line) {
        if (verb == "RCPT") {
            std::string addr = extract_addr(line);
            if (!validate_rcpt(fd, sess, addr)) return false;
            sess->msg.rcpt_to.push_back(addr);
            send_reply(fd, sess, "250 2.1.5 OK\r\n");
            return false;
        }
        if (verb == "DATA") {
            sess->state = SmtpState::DATA_INIT;
            send_reply(fd, sess, "354 Start mail input; end with <CRLF>.<CRLF>\r\n",
                [this, fd, sess]() {
                    sess->state = SmtpState::DATA_BODY;
                    loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                        on_data(fd, sess, d, l);
                    });
                });
            return false;
        }
        return error_reply(fd, sess, "503 5.5.1 Need DATA or more RCPT\r\n");
    }

    bool handle_data_init(int /*fd*/, std::shared_ptr<SmtpSession> /*sess*/,
                          const std::string& /*verb*/, const std::string& /*line*/) {
        // Should not reach here — DATA_INIT transitions to DATA_BODY via callback
        return true;
    }

    bool handle_data_body(int fd, std::shared_ptr<SmtpSession> sess,
                          const std::string& line) {
        // RFC 5321 §4.5.2 — lone "." terminates message
        if (line == ".") {
            if (sess->msg.data.size() > smtp_max_msg()) {
                send_reply(fd, sess, "552 5.3.4 Message too large\r\n",
                    [this, fd, sess]() {
                        sess->reset_envelope();
                        loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                            on_data(fd, sess, d, l);
                        });
                    });
                return false;
            }
            // Hand off to worker thread
            WorkItem item;
            item.msg     = sess->msg;
            item.fd      = fd;
            item.handler = handler;
            item.loop    = loop.get();
            item.session = sess;
            pool.enqueue(std::move(item));
            return false;
        }
        // RFC 5321 §4.5.2 dot-unstuffing: leading "." removed
        if (!line.empty() && line[0] == '.') {
            sess->msg.data += line.substr(1) + "\r\n";
        } else {
            sess->msg.data += line + "\r\n";
        }
        if (sess->msg.data.size() > smtp_max_msg()) {
            send_reply(fd, sess, "552 5.3.4 Message too large\r\n",
                [this, fd, sess]() {
                    sess->reset_envelope();
                    loop->async_read(fd, [this, fd, sess](const char* d, size_t l) {
                        on_data(fd, sess, d, l);
                    });
                });
            return false;
        }
        return true; // continue accumulating
    }

    bool error_reply(int fd, std::shared_ptr<SmtpSession> sess,
                     const std::string& reply) {
        sess->error_count++;
        if (sess->error_count >= smtp_max_errors()) {
            send_reply(fd, sess, "421 4.7.0 Too many errors\r\n",
                [this, fd]{ close_session(fd); });
            return false;
        }
        send_reply(fd, sess, reply);
        return false;
    }
};

// ── SmtpServer public API ─────────────────────────────────────────────────────

SmtpServer::SmtpServer(int port_smtp, int port_sub, int port_smtps,
                       int workers, SmtpHandler handler)
    : impl_(std::make_unique<Impl>())
{
    impl_->loop    = EventLoop::create();
    impl_->handler = std::move(handler);
    impl_->ssl_ctx = make_smtp_ssl_ctx(); // null if LOOK_SMTP_CERT/KEY not set
    impl_->pool.start(workers);

    auto bind_port = [&](int port, bool submission) {
        if (port <= 0) return;
        int fd = Impl::make_server_fd(port);
        if (fd < 0) {
            Logger::instance().log(LogLevel::LOG_ERROR, "smtp",
                "cannot bind port " + std::to_string(port));
            return;
        }
        bool sub = submission;
        impl_->loop->listen(fd, [this, sub](int client_fd) {
            impl_->on_accept(client_fd, sub);
        });
        Logger::instance().log(LogLevel::LOG_INFO, "smtp",
            "listening on :" + std::to_string(port));
    };

    bind_port(port_smtp,  false); // :25  MTA-to-MTA
    bind_port(port_sub,   true);  // :587 authenticated submission
    // :465 implicit TLS — placeholder, wired when TLS layer is added
    (void)port_smtps;
}

SmtpServer::~SmtpServer() {
    impl_->pool.shutdown();
    if (impl_->ssl_ctx) { SSL_CTX_free(impl_->ssl_ctx); impl_->ssl_ctx = nullptr; }
}

void SmtpServer::run()  { impl_->loop->run();  }
void SmtpServer::stop() { impl_->loop->stop(); }

// ── Maildir delivery ──────────────────────────────────────────────────────────
// Maildir format: base_dir/<mailbox>/{new,cur,tmp}/
// Each message = tmp/<unique> → rename to new/<unique> (atomic)

std::string deliver_maildir(const std::string& base_dir,
                            const std::string& mailbox,
                            const SmtpMessage& msg) {
    fs::path mbox = fs::path(base_dir) / mailbox;
    fs::create_directories(mbox / "new");
    fs::create_directories(mbox / "cur");
    fs::create_directories(mbox / "tmp");

    // Unique filename: timestamp.pid.random:2, (Maildir standard)
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                       now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << ts << "." << std::hex << dist(rng);
    std::string unique = oss.str();

    fs::path tmp_path = mbox / "tmp" / unique;
    fs::path new_path = mbox / "new" / unique;

    {
#if defined(__linux__) || defined(__APPLE__)
        // Use POSIX open/write/fdatasync for crash safety (RFC-correct Maildir)
        int fd_out = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd_out < 0)
            throw std::runtime_error("maildir: cannot create " + tmp_path.string());

        auto write_str = [&](const std::string& s) {
            const char* p = s.data();
            size_t rem = s.size();
            while (rem > 0) {
                ssize_t n = ::write(fd_out, p, rem);
                if (n <= 0) { ::close(fd_out); throw std::runtime_error("maildir: write error"); }
                p += n; rem -= (size_t)n;
            }
        };
        write_str("Return-Path: <" + msg.mail_from + ">\r\n");
        write_str("Received: from " + msg.remote_ip + "\r\n");
        write_str(msg.data);

        if (::fdatasync(fd_out) != 0) {
            ::close(fd_out);
            throw std::runtime_error("maildir: fdatasync failed");
        }
        ::close(fd_out);
#else
        // Windows: no fdatasync, use FlushFileBuffers via ofstream workaround
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f) throw std::runtime_error("maildir: cannot write to " + tmp_path.string());
        f << "Return-Path: <" << msg.mail_from << ">\r\n";
        f << "Received: from " << msg.remote_ip << "\r\n";
        f << msg.data;
        f.flush();
        f.close();
#endif
    }

    fs::rename(tmp_path, new_path); // atomic on POSIX
    return new_path.filename().string();
}

} // namespace look

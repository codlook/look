// LOOK IMAP4rev1 Server (RFC 3501) — Milestone 1
//
// M1 scope: greeting · CAPABILITY · LOGIN · LOGOUT · NOOP.
// Model: thread-per-connection (blocking), bounded by MAX_CONN. IMAP sessions
// are long-lived; this model is easy to verify and a natural base for IDLE (M6).
//
// SECURITY (lessons learned applied from the start):
//   • Line length cap (RESP2 read_line -> OOM lesson)
//   • Literal {N} size cap (RESP2 read_bulk -> OOM lesson)
//   • std::stoul guarded (protocol parser stoi crash lesson)
//   • Failed-login delay (slows brute force)
//   • Concurrent connection + per-session error limit (DoS)

#include "look/imap_server.h"
#if !defined(_WIN32)
#include <poll.h>
#endif
#include "look/imap_parse.h"   // pure parse seam: imap_parse_command / imap_split_two / imap_mailbox_name_ok
#include "look/logger.h"

#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <vector>
#include <system_error>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <ctime>
#include <random>

#ifndef OPENSSL_NO_DEPRECATED_3_0
#  define OPENSSL_NO_DEPRECATED_3_0
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace fs = std::filesystem;

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
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

namespace look {

// ── TLS: the active connection's SSL object (thread-per-connection -> thread_local) ──
// Each session runs in its own thread; the I/O helpers check this pointer
// and use SSL_write/SSL_read instead of ::send/::recv. This way the 40+ call
// sites become TLS-aware without changes.
// fiber-ok: the IMAP session runs in its own thread (a thread per connection, no fibers)
static thread_local SSL* g_tls = nullptr;

// ── Env configuration (security limits) ─────────────────────────────────────
static long env_long(const char* name, long def) {
    const char* e = std::getenv(name);
    if (e && *e) { char* end = nullptr; long v = std::strtol(e, &end, 10); if (end != e && v > 0) return v; }
    return def;
}
static size_t imap_max_conn()    { return (size_t)env_long("LOOK_IMAP_MAX_CONN",    1000); }
static size_t imap_max_line()    { return (size_t)env_long("LOOK_IMAP_MAX_LINE",    8 * 1024); }
static size_t imap_max_literal() { return (size_t)env_long("LOOK_IMAP_MAX_LITERAL", 32 * 1024 * 1024); }
static int    imap_max_errors()  { return (int)env_long("LOOK_IMAP_MAX_ERRORS",     5); }
static int    imap_auth_delay_ms(){ return (int)env_long("LOOK_IMAP_AUTH_DELAY_MS", 500); }

// ── Impl ──────────────────────────────────────────────────────────────────────
struct ImapServer::Impl {
    int              port_imap;
    int              port_imaps;
    int              workers;   // not used in M1 (thread-per-conn); for M6+
    ImapAuthHandler  auth;

    int                 listen_fd = -1;      // 143 (STARTTLS)
    int                 listen_fd_tls = -1;  // 993 (implicit TLS / IMAPS)
    std::atomic<bool>   running{false};
    std::atomic<size_t> conn_count{0};
    std::thread         accept_thread;
    std::thread         accept_thread_tls;
    SSL_CTX*            ssl_ctx = nullptr;   // null = TLS not configured

    // ── TLS server context (same pattern as SMTP; certificate from env) ─────────
    // LOOK_IMAP_CERT/LOOK_IMAP_KEY, otherwise LOOK_SMTP_CERT/LOOK_SMTP_KEY are shared.
    static SSL_CTX* make_ssl_ctx() {
        const char* cert = std::getenv("LOOK_IMAP_CERT");
        const char* key  = std::getenv("LOOK_IMAP_KEY");
        if (!cert || !key) { cert = std::getenv("LOOK_SMTP_CERT"); key = std::getenv("LOOK_SMTP_KEY"); }
        if (!cert || !key || !*cert || !*key) return nullptr;   // TLS not configured
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) return nullptr;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);     // TLS 1.2 floor
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                  SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            Logger::instance().log(LogLevel::LOG_ERROR, "IMAP",
                "TLS: could not load certificate/key — TLS disabled");
            SSL_CTX_free(ctx);
            return nullptr;
        }
        return ctx;
    }

    // TLS handshake: SSL_accept on the fd, g_tls is set if it succeeds.
    bool tls_handshake(int fd) {
        if (!ssl_ctx) return false;
        SSL* ssl = SSL_new(ssl_ctx);
        if (!ssl) return false;
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) { SSL_free(ssl); return false; }
        g_tls = ssl;
        return true;
    }

    // ── Secure listening socket (dual-stack IPv6, WITH IPv4 FALLBACK) ───────────────
    // OLD BUG: only AF_INET6 was tried. In environments where IPv6 support is NOT COMPILED IN/disabled
    // (hardened VPS, some container setups) `socket()`
    // fails with EAFNOSUPPORT and the mail server DOES NOT START AT ALL — and silently,
    // because http_main still prints the "started" log.
    // The HTTP server works in those environments because it uses AF_INET, while the mail
    // servers did not; the difference came from here.
    static int make_server_fd(int port) {
#if defined(_WIN32)
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        int fd = (int)socket(AF_INET6, SOCK_STREAM, 0);
#else
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
#endif
        if (fd < 0) return make_server_fd_v4(port);   // no IPv6 -> try IPv4
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no));  // IPv4+IPv6
        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons((uint16_t)port);
        addr.sin6_addr   = in6addr_any;
        // bind can fail too (IPv6 interface down but the socket opens) -> IPv4
        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return make_server_fd_v4(port); }
        if (::listen(fd, 128) < 0) { close(fd); return -1; }
        return fd;
    }

    // IPv4-only listening socket — fallback when IPv6 is unavailable.
    static int make_server_fd_v4(int port) {
#if defined(_WIN32)
        int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
#else
        int fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
        if (fd < 0) return -1;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        struct sockaddr_in a4{};
        a4.sin_family      = AF_INET;
        a4.sin_port        = htons((uint16_t)port);
        a4.sin_addr.s_addr = INADDR_ANY;
        if (::bind(fd, (struct sockaddr*)&a4, sizeof(a4)) < 0) { close(fd); return -1; }
        if (::listen(fd, 128) < 0) { close(fd); return -1; }
        return fd;
    }

    // ── Raw I/O ──────────────────────────────────────────────────────────────
    static bool send_all(int fd, const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            if (g_tls) {
                int n = SSL_write(g_tls, s.data() + sent, (int)(s.size() - sent));
                if (n <= 0) return false;
                sent += (size_t)n;
                continue;
            }
#if defined(_WIN32)
            int n = ::send(fd, s.data() + sent, (int)(s.size() - sent), 0);
#else
            ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
#endif
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    // Is the socket readable within ms milliseconds? (to watch both DONE and new mail
    // in IDLE). select() — shared by Winsock+POSIX. With TLS, data in the buffer also counts as
    // "readable" (SSL_pending). Returns: 1=ready, 0=timeout, -1=error.
    static int wait_readable(int fd, int ms) {
        if (g_tls && SSL_pending(g_tls) > 0) return 1;
#if defined(_WIN32)
        fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
        struct timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        int r = ::select(fd + 1, &rs, nullptr, nullptr, &tv);
#else
        // NOT select(): fd_set only holds descriptors below 1024; FD_SET overflows memory for larger ones.
        struct pollfd pf{}; pf.fd = fd; pf.events = POLLIN;
        int r = ::poll(&pf, 1, ms);
#endif
        return r;
    }

    // TLS-aware single-block read (::recv or SSL_read).
    static ssize_t conn_recv(int fd, char* buf, size_t len) {
        if (g_tls) return SSL_read(g_tls, buf, (int)len);
#if defined(_WIN32)
        return ::recv(fd, buf, (int)len, 0);
#else
        return ::recv(fd, buf, len, 0);
#endif
    }

    // Slowloris protection: idle recv timeout. An attacker must not be able to open a connection and send no/very
    // slow data — the thread does not block indefinitely. LOOK_IMAP_IDLE_TIMEOUT
    // (seconds, default 1800 = 30 min, the RFC autologout recommendation). Together with MAX_CONN
    // total resources are bounded.
    static void set_recv_timeout(int fd) {
        long secs = env_long("LOOK_IMAP_IDLE_TIMEOUT", 1800);
#if defined(_WIN32)
        DWORD ms = (DWORD)secs * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
#else
        struct timeval tv{}; tv.tv_sec = secs; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    // Read exactly n bytes (for literal data). Bounded — n must have been validated by the caller
    // against MAX_LITERAL. false -> connection/timeout.
    static bool read_exact(int fd, size_t n, std::string& out) {
        out.clear();
        out.reserve(n);
        char buf[8192];
        size_t got = 0;
        while (got < n) {
            size_t want = std::min(sizeof(buf), n - got);
            ssize_t r = conn_recv(fd, buf, want);
            if (r <= 0) return false;
            out.append(buf, (size_t)r);
            got += (size_t)r;
        }
        return true;
    }

    // Write atomically to the Maildir: tmp/<unique> -> new/<unique>[:2,<flags>] rename.
    // Crash-safe (a partial message is never visible in new/). Unique name: epoch.counter.random.
    // Returns: false = failed.
    static bool maildir_append(const std::string& box, const std::string& mflags,
                               const std::string& data) {
        std::error_code ec;
        fs::create_directories(fs::path(box) / "tmp", ec);
        fs::create_directories(fs::path(box) / "new", ec);
        fs::create_directories(fs::path(box) / "cur", ec);
        static std::atomic<uint64_t> counter{0};
        std::random_device rd;
        std::string unique = std::to_string((uint64_t)std::time(nullptr)) + "." +
                             std::to_string(counter.fetch_add(1)) + "." +
                             std::to_string(rd());
        fs::path tmp = fs::path(box) / "tmp" / unique;
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f) return false;
            f.write(data.data(), (std::streamsize)data.size());
            if (!f) { fs::remove(tmp, ec); return false; }
        }
        std::string nf = normalize_flags(mflags);
        // With flags straight to cur/ (read/flagged), otherwise to new/ (RFC Maildir)
        fs::path dest = nf.empty() ? (fs::path(box) / "new" / unique)
                                   : (fs::path(box) / "cur" / (unique + ":2," + nf));
        fs::rename(tmp, dest, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    }

    // Read one CRLF-terminated command line. The line is bounded by MAX_LINE.
    // false -> connection closed or limit exceeded.
    static bool read_line(int fd, std::string& out) {
        out.clear();
        const size_t max_line = imap_max_line();
        char c;
        while (true) {
            ssize_t n = conn_recv(fd, &c, 1);
            if (n <= 0) return false;
            if (c == '\r') continue;
            if (c == '\n') return true;
            out += c;
            if (out.size() > max_line) return false;  // line bomb -> cut off
        }
    }

    // parse_command / split_two -> look/imap_parse.h (imap_parse_command / imap_split_two).
    // The pure string parsers were moved to the seam (fuzz + table test; single definition, no drift).

    // ── SECURITY: mailbox name -> Maildir path (path traversal protection) ───────
    // A user cannot say "SELECT ../../etc" or "SELECT /etc/passwd".
    // INBOX -> the user's root Maildir; others are verified canonically
    // under the root. Every name that escapes the root is rejected.
    // Returns: a valid path; empty = invalid/traversal.
    static std::string resolve_mailbox(const std::string& root, const std::string& name) {
        if (root.empty()) return "";
        // PURE string gate (quote stripping + traversal/injection guard) -> seam.
        // The documented past bug (`INBOX; rm -rf /` ACCEPTED) and every separator/control rejection
        // is locked there with a table + fuzz. INBOX/empty -> root Maildir.
        std::string mb;
        if (!imap_mailbox_name_ok(name, mb)) return "";
        if (mb == "INBOX") return root;

        std::error_code ec;
        fs::path root_c = fs::weakly_canonical(fs::path(root), ec);
        if (ec) return "";
        // Maildir++ subfolder: "Sent" -> root/.Sent  (dot prefix)
        fs::path target = fs::path(root) / ("." + mb);
        fs::path target_c = fs::weakly_canonical(target, ec);
        if (ec) return "";
        // target_c must be strictly under root_c (traversal lock). A plain
        // string prefix is NOT ENOUGH (the "/root" prefix matches "/root-evil"); the separator
        // boundary is also required — containment that is correct on its own, without relying on the ".." rejection above
        // (same discipline as installer/template).
        auto rs = root_c.string(), ts = target_c.string();
        bool within = ts.size() >= rs.size() && ts.compare(0, rs.size(), rs) == 0 &&
                      (ts.size() == rs.size() || ts[rs.size()] == '/' || ts[rs.size()] == '\\');
        if (!within) return "";
        // A mailbox that does NOT exist used to return `OK (0 EXISTS)` — per RFC 3501 §6.3.1,
        // SELECT/EXAMINE must return `NO` for a nonexistent mailbox. `weakly_canonical`
        // also resolves nonexistent paths, so the distinction was lost: the client would
        // SELECT "Sent", get OK and see an empty box — although the box does not exist at all.
        // (INBOX returns early above; it always exists.)
        if (!fs::is_directory(target_c, ec) || ec) return "";
        return target_c.string();
    }

    // Count messages in the Maildir: files under new/ + cur/. recent = new/.
    static void count_maildir(const std::string& box, size_t& total, size_t& recent) {
        total = 0; recent = 0;
        std::error_code ec;
        for (const char* sub : {"new", "cur"}) {
            fs::path d = fs::path(box) / sub;
            if (!fs::is_directory(d, ec)) continue;
            for (auto it = fs::directory_iterator(d, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    total++;
                    if (std::strcmp(sub, "new") == 0) recent++;
                }
            }
        }
    }

    // List the user's mailboxes: root = INBOX, .X folders = sub-mailboxes.
    static std::vector<std::string> list_mailboxes(const std::string& root) {
        std::vector<std::string> boxes;
        boxes.push_back("INBOX");
        std::error_code ec;
        for (auto it = fs::directory_iterator(root, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            std::string n = it->path().filename().string();
            if (n.size() > 1 && n[0] == '.' && n != "." && n != "..")
                boxes.push_back(n.substr(1));  // ".Sent" → "Sent"
        }
        return boxes;
    }

    // Collect the mailbox's message files in a deterministic order (for stable
    // sequence numbers). new/ and cur/ combined, sorted by file name.
    static std::vector<std::string> build_messages(const std::string& box) {
        std::vector<std::string> files;
        std::error_code ec;
        for (const char* sub : {"cur", "new"}) {   // cur first (read ones), then new
            fs::path d = fs::path(box) / sub;
            if (!fs::is_directory(d, ec)) continue;
            for (auto it = fs::directory_iterator(d, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back(it->path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // Read the file into memory (size bounded by MAX_LITERAL — protection against OOM from a huge message)
    static bool read_file_capped(const std::string& path, std::string& out) {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec || sz > imap_max_literal()) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return true;
    }

    // RFC 5322: split header and body (\r\n\r\n or \n\n).
    static void split_header_body(const std::string& msg, std::string& hdr, std::string& body) {
        size_t p = msg.find("\r\n\r\n");
        size_t skip = 4;
        if (p == std::string::npos) { p = msg.find("\n\n"); skip = 2; }
        if (p == std::string::npos) { hdr = msg; body.clear(); return; }
        hdr  = msg.substr(0, p + (skip == 4 ? 2 : 1));  // header end including CRLF
        body = msg.substr(p + skip);
    }

    // parse a seq-set ("1", "1:3", "1:*", "*") -> [lo, hi] (1-based, inclusive).
    static bool parse_seqset(const std::string& s, size_t count, size_t& lo, size_t& hi) {
        if (count == 0) return false;
        auto num = [&](const std::string& t, size_t def) -> size_t {
            if (t == "*") return count;
            try { long v = std::stol(t); return (v < 1) ? def : (size_t)v; } catch (...) { return def; }
        };
        size_t colon = s.find(':');
        if (colon == std::string::npos) { lo = hi = num(s, 1); }
        else { lo = num(s.substr(0, colon), 1); hi = num(s.substr(colon + 1), count); }
        if (lo > hi) std::swap(lo, hi);
        if (lo < 1) lo = 1;
        if (hi > count) hi = count;
        return lo <= hi;
    }

    // ── Maildir flag encoding (RFC + Maildir standard) ─────────────────────
    // Maildir file name: "<base>:2,<flags>" — flags UPPERCASE, in ALPHABETICAL order.
    // IMAP → Maildir: \Draft=D \Flagged=F \Answered=R \Seen=S \Deleted=T
    // A message under new/ has no flags; when flags are given it is moved to cur/.

    // Extract the current Maildir flag set from the file name (":2,XYZ" -> "XYZ").
    static std::string maildir_flags(const std::string& path) {
        std::string name = fs::path(path).filename().string();
        size_t p = name.find(":2,");
        if (p == std::string::npos) return "";
        std::string f = name.substr(p + 3);
        // the info part is only flag letters — on the safe side take only the known ones
        std::string out;
        for (char c : f) if (std::strchr("DFPRST", c)) out += c;
        return out;
    }

    // IMAP flag name -> Maildir letter (unknown -> 0).
    static char imap_to_maildir_flag(const std::string& f) {
        std::string u = f; for (char& c : u) c = (char)std::toupper((unsigned char)c);
        if (u == "\\SEEN")     return 'S';
        if (u == "\\ANSWERED") return 'R';
        if (u == "\\FLAGGED")  return 'F';
        if (u == "\\DELETED")  return 'T';
        if (u == "\\DRAFT")    return 'D';
        return 0;  // \Recent and custom flags are not stored in Maildir
    }

    // Turn the Maildir letter set into an alphabetical, de-duplicated string (Maildir rule).
    static std::string normalize_flags(const std::string& in) {
        bool seen[256] = {false};
        for (unsigned char c : in) if (std::strchr("DFPRST", c)) seen[c] = true;
        std::string out;
        for (char c : std::string("DFPRST")) if (seen[(unsigned char)c]) out += c;
        return out;
    }

    // Turn the Maildir flag set into the IMAP FLAGS representation ("(\Seen \Deleted)").
    static std::string maildir_to_imap_flags(const std::string& mf) {
        std::string out = "(";
        bool first = true;
        auto add = [&](char c, const char* name) {
            if (mf.find(c) != std::string::npos) { if (!first) out += " "; out += name; first = false; }
        };
        add('S', "\\Seen"); add('R', "\\Answered"); add('F', "\\Flagged");
        add('T', "\\Deleted"); add('D', "\\Draft");
        out += ")";
        return out;
    }

    // Rename the file with the new flag set (new/ -> cur/ if needed).
    // Returns: the new path; empty = failed (concurrent delete/move -> skip silently).
    static std::string apply_flags(const std::string& path, const std::string& new_flags) {
        std::error_code ec;
        fs::path pp(path);
        std::string name = pp.filename().string();
        std::string base = name;
        size_t p = name.find(":2,");
        if (p != std::string::npos) base = name.substr(0, p);
        std::string nf = normalize_flags(new_flags);
        std::string newname = base + ":2," + nf;
        // Target folder: cur/ if flags were given, otherwise where it already is.
        fs::path parent = pp.parent_path();
        bool in_new = parent.filename() == "new";
        fs::path dest_dir = in_new ? (parent.parent_path() / "cur") : parent;
        fs::path dest = dest_dir / newname;
        if (dest == pp) return path;  // no change
        fs::rename(pp, dest, ec);     // atomic on the same FS
        if (ec) return "";            // source is gone (concurrent) -> skip
        return dest.string();
    }

    // ── Session loop (one connection = one thread) ───────────────────────────
    void run_session(int fd, bool implicit_tls) {
        struct ConnGuard {
            std::atomic<size_t>& c; int fd;
            ~ConnGuard() {
                if (g_tls) { SSL_shutdown(g_tls); SSL_free(g_tls); g_tls = nullptr; }
                c--; close(fd);
            }
        } guard{conn_count, fd};

        set_recv_timeout(fd);   // Slowloris protection

        // IMAPS (993): TLS handshake as soon as the connection opens (implicit TLS).
        if (implicit_tls) {
            if (!tls_handshake(fd)) return;   // handshake failed -> drop
        }

        // While TLS is active, STARTTLS is not advertised in CAPABILITY; otherwise it is.
        auto cap_line = [&]() -> std::string {
            std::string c = "IMAP4rev1 IDLE";
            if (ssl_ctx && !g_tls) c += " STARTTLS LOGINDISABLED";
            return c;
        };

        // Greeting
        if (!send_all(fd, "* OK [CAPABILITY " + cap_line() + "] LOOK IMAP ready\r\n")) return;

        bool authenticated = false;
        std::string maildir_root;      // the user's Maildir root after LOGIN
        std::string selected;          // selected mailbox path ("" = none selected)
        // STABLE message snapshot — taken at SELECT. Sequence numbers
        // index into it (RFC 3501): FIXED within the session, only EXPUNGE changes them.
        // When STORE renames the file, the path is updated at the same index (NO re-sort).
        std::vector<std::string> messages;
        int  errors = 0;
        std::string line, tag, cmd, args;

        while (running.load()) {
            if (!read_line(fd, line)) break;
            imap_parse_command(line, tag, cmd, args);
            if (tag.empty()) { send_all(fd, "* BAD empty command\r\n"); if (++errors >= imap_max_errors()) break; continue; }

            if (cmd == "CAPABILITY") {
                send_all(fd, "* CAPABILITY " + cap_line() + "\r\n" + tag + " OK CAPABILITY completed\r\n");
            }
            else if (cmd == "STARTTLS") {
                if (g_tls) { send_all(fd, tag + " NO TLS already active\r\n"); continue; }
                if (!ssl_ctx) { send_all(fd, tag + " NO STARTTLS not configured\r\n"); continue; }
                // Send OK first (plaintext), then handshake (RFC 3501 6.2.1).
                if (!send_all(fd, tag + " OK beginning TLS handshake\r\n")) break;
                if (!tls_handshake(fd)) break;   // failed -> drop the connection
                // Security (RFC 3501): state from before TLS is discarded — reset the identity.
                authenticated = false; maildir_root.clear(); selected.clear(); messages.clear();
            }
            else if (cmd == "NOOP") {
                send_all(fd, tag + " OK NOOP completed\r\n");
            }
            // ── M6: IDLE — live push for new mail (RFC 2177) ──────────────
            else if (cmd == "IDLE") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO SELECT required first\r\n"); continue; }
                if (!send_all(fd, "+ idling\r\n")) break;
                // Sequence STABILITY: new messages are APPENDED to the snapshot (existing
                // indexes do not change). Scan new/ and append the ones not yet known at the end.
                bool broke = false;
                int tick_ms = (int)env_long("LOOK_IMAP_IDLE_TICK_MS", 15000);
                while (running.load()) {
                    int rr = wait_readable(fd, tick_ms);   // periodic mailbox scan
                    if (rr < 0) { broke = true; break; }  // select error -> drop the session
                    if (rr == 0) {
                        // Timeout -> is there a new message in the mailbox?
                        std::vector<std::string> fresh = build_messages(selected);
                        std::vector<std::string> added;
                        for (auto& p : fresh)
                            if (std::find(messages.begin(), messages.end(), p) == messages.end())
                                added.push_back(p);
                        if (!added.empty()) {
                            for (auto& p : added) messages.push_back(p);  // append at the end -> seq stays fixed
                            size_t recent = 0;
                            for (auto& m : messages) if (m.find("/new/") != std::string::npos) recent++;
                            if (!send_all(fd, "* " + std::to_string(messages.size()) + " EXISTS\r\n"
                                              "* " + std::to_string(recent) + " RECENT\r\n")) { broke = true; break; }
                        }
                        continue;
                    }
                    // Readable -> a line from the client (only "DONE" is expected)
                    std::string dline;
                    if (!read_line(fd, dline)) { broke = true; break; }
                    std::string du = dline;
                    for (char& c : du) c = (char)std::toupper((unsigned char)c);
                    if (du == "DONE") break;
                    // RFC: no command other than DONE inside IDLE -> protocol error
                    if (!send_all(fd, tag + " BAD DONE required to end IDLE\r\n")) { broke = true; break; }
                    broke = true; break;
                }
                if (broke) break;
                send_all(fd, tag + " OK IDLE completed\r\n");
            }
            else if (cmd == "LOGOUT") {
                send_all(fd, "* BYE LOOK IMAP session closing\r\n" + tag + " OK LOGOUT completed\r\n");
                break;
            }
            else if (cmd == "LOGIN") {
                // Security: if TLS is on, plaintext LOGIN is rejected (LOGINDISABLED).
                if (ssl_ctx && !g_tls) {
                    send_all(fd, tag + " NO [PRIVACYREQUIRED] STARTTLS required first\r\n");
                    continue;
                }
                std::string user, pass;
                imap_split_two(args, user, pass);
                ImapAuthResult r = auth ? auth(user, pass) : ImapAuthResult{};
                if (r.ok) {
                    authenticated = true;
                    maildir_root  = r.maildir_path;
                    send_all(fd, tag + " OK LOGIN successful\r\n");
                } else {
                    // Brute-force slowdown — fixed delay on a failed attempt
                    std::this_thread::sleep_for(std::chrono::milliseconds(imap_auth_delay_ms()));
                    send_all(fd, tag + " NO LOGIN failed\r\n");
                    if (++errors >= imap_max_errors()) break;
                }
            }
            // ── M2: SELECT / EXAMINE — select a mailbox, count messages ────────────────
            else if (cmd == "SELECT" || cmd == "EXAMINE") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                std::string box = resolve_mailbox(maildir_root, args);
                if (box.empty()) { send_all(fd, tag + " NO invalid mailbox name\r\n"); continue; }
                // Take a stable snapshot — sequence numbers are fixed from this moment on
                selected = box;
                messages = build_messages(box);
                size_t total = messages.size();
                size_t recent = 0;
                for (auto& m : messages) if (m.find("/new/") != std::string::npos) recent++;
                std::string ro = (cmd == "EXAMINE") ? "READ-ONLY" : "READ-WRITE";
                std::string resp =
                    "* " + std::to_string(total)  + " EXISTS\r\n"
                    "* " + std::to_string(recent) + " RECENT\r\n"
                    "* OK [UIDVALIDITY 1] UID validity\r\n"
                    "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
                    "* OK [PERMANENTFLAGS (\\Seen \\Deleted)] permanent flags\r\n"
                    + tag + " OK [" + ro + "] " + cmd + " completed\r\n";
                send_all(fd, resp);
            }
            // ── M2: LIST — list mailboxes ──────────────────────────────
            else if (cmd == "LIST" || cmd == "LSUB") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                std::string resp;
                for (auto& b : list_mailboxes(maildir_root))
                    resp += "* " + cmd + " (\\HasNoChildren) \"/\" \"" + b + "\"\r\n";
                resp += tag + " OK " + cmd + " completed\r\n";
                send_all(fd, resp);
            }
            // ── M2: STATUS — mailbox status ──────────────────────────────────
            else if (cmd == "STATUS") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                std::string mbname = args;
                size_t sp = mbname.find(' ');
                if (sp != std::string::npos) mbname = mbname.substr(0, sp);  // first arg = mailbox
                std::string box = resolve_mailbox(maildir_root, mbname);
                if (box.empty()) { send_all(fd, tag + " NO invalid mailbox\r\n"); continue; }
                size_t total = 0, recent = 0;
                count_maildir(box, total, recent);
                if (mbname.size() >= 2 && mbname.front()=='"' && mbname.back()=='"')
                    mbname = mbname.substr(1, mbname.size()-2);
                send_all(fd, "* STATUS \"" + mbname + "\" (MESSAGES " + std::to_string(total) +
                             " RECENT " + std::to_string(recent) + ")\r\n" +
                             tag + " OK STATUS completed\r\n");
            }
            // ── M3: FETCH — read message content (headers/body/flags/size/uid) ──
            else if (cmd == "FETCH") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO SELECT required first\r\n"); continue; }
                // args: "<seq-set> <items>"  (items parenthesized or single)
                size_t sp = args.find(' ');
                if (sp == std::string::npos) { send_all(fd, tag + " BAD FETCH argument\r\n"); continue; }
                std::string seqset = args.substr(0, sp);
                std::string items  = args.substr(sp + 1);
                for (char& ch : items) ch = (char)std::toupper((unsigned char)ch);

                // The CAPABILITY check must come BEFORE the DATA check: even if the
                // sequence-set matches nothing in an empty mailbox, the client must get the answer to
                // "is UID supported". (Otherwise `FETCH 1 (UID)` on an empty INBOX returned "OK (empty)"
                // and the client thought UID was supported.)
                //
                // UID data item — M1 has NO PERSISTENT UID, so it is REJECTED.
                //
                // OLD BUG (measured): when `UID` was requested the current SEQUENCE number
                // was returned. RFC 3501 §2.3.1.1 requires UIDs to be persistent:
                //   3 messages -> UID 1,2,3 -> the middle one is deleted -> the rest get UID 1,2
                //   (should be 1,3). UID 2 is now a DIFFERENT message: first M1, then M2.
                // A client/webmail that caches this keeps showing the deleted message
                // and when opening it gets another message's content — SILENT DATA CORRUPTION.
                // Until a persistent UID store (dovecot-uidlist / the `,U=N` pattern) arrives,
                // a LOUD ERROR is the right behavior: better to give no UID than a wrong one.
                if (items.find("UID") != std::string::npos) {
                    send_all(fd, tag + " NO [CANNOT] UID not supported — no persistent UID "
                                       "(Milestone 1). Use sequence numbers.\r\n");
                    continue;
                }
                // Unsupported item: substring search used to ignore it —
                // a client asking for BODY[HEADER.FIELDS (FROM)], BODY.PEEK[], ENVELOPE ... silently got an
                // EMPTY FETCH. Now every item must be recognized; otherwise an explicit BAD.
                {
                    std::string list = items;
                    while (!list.empty() && list.front() == ' ') list.erase(0, 1);
                    while (!list.empty() && list.back() == ' ') list.pop_back();
                    if (list.size() >= 2 && list.front() == '(' && list.back() == ')')
                        list = list.substr(1, list.size() - 2);
                    std::vector<std::string> toks; std::string cur; int depth = 0;
                    for (char ch : list) {
                        if (ch == '[' || ch == '(') depth++;
                        else if ((ch == ']' || ch == ')') && depth > 0) depth--;
                        if (ch == ' ' && depth == 0) { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
                        else cur += ch;
                    }
                    if (!cur.empty()) toks.push_back(cur);
                    static const char* const kOk[] = { "FLAGS", "RFC822.SIZE", "RFC822", "RFC822.HEADER",
                                                       "BODY[]", "BODY[HEADER]", "BODY[TEXT]" };
                    std::string bad;
                    for (const auto& t : toks) {
                        bool ok = false;
                        for (const char* k : kOk) if (t == k) { ok = true; break; }
                        if (!ok) { bad = t; break; }
                    }
                    if (toks.empty() || !bad.empty()) {
                        send_all(fd, tag + " BAD FETCH item not supported: " + (toks.empty() ? std::string("(none)") : bad) +
                                     " (supported: FLAGS RFC822.SIZE RFC822 RFC822.HEADER BODY[] BODY[HEADER] BODY[TEXT])\r\n");
                        continue;
                    }
                }

                auto& msgs = messages;   // stable snapshot
                size_t lo = 0, hi = 0;
                if (!parse_seqset(seqset, msgs.size(), lo, hi)) {
                    send_all(fd, tag + " OK FETCH completed (empty)\r\n"); continue;
                }
                bool want_flags = items.find("FLAGS") != std::string::npos;
                bool want_size  = items.find("RFC822.SIZE") != std::string::npos;
                // "RFC822" asks for the full body; it does not include "RFC822.SIZE"/"RFC822.HEADER".
                bool rfc822_full = (items.find("RFC822") != std::string::npos &&
                                    items.find("RFC822.") == std::string::npos);
                bool want_hdr   = items.find("BODY[HEADER]")!= std::string::npos || items.find("RFC822.HEADER") != std::string::npos;
                bool want_text  = items.find("BODY[TEXT]")  != std::string::npos;
                bool want_full  = items.find("BODY[]") != std::string::npos || rfc822_full;

                // Process + send each message ONE BY ONE — memory is bounded to a single message (OOM protection)
                for (size_t i = lo; i <= hi; i++) {
                    std::string raw;
                    if (!read_file_capped(msgs[i-1], raw)) continue;  // huge/unreadable message -> skip
                    std::string hdr, body; split_header_body(raw, hdr, body);

                    std::string parts;
                    // Report ALL flags (consistent with STORE) — not only \Seen
                    if (want_flags) parts += "FLAGS " + maildir_to_imap_flags(maildir_flags(msgs[i-1])) + " ";
                    // (UID is rejected above — giving the sequence
                    //  number as the UID while there is no persistent UID would have been silent data corruption.)
                    if (want_size)  parts += "RFC822.SIZE " + std::to_string(raw.size()) + " ";
                    std::string literal;
                    if (want_full)      { parts += "BODY[] ";       literal = raw;  }
                    else if (want_hdr)  { parts += "BODY[HEADER] "; literal = hdr;  }
                    else if (want_text) { parts += "BODY[TEXT] ";   literal = body; }

                    std::string resp = "* " + std::to_string(i) + " FETCH (" ;
                    // trim the trailing space at the end of parts
                    while (!parts.empty() && parts.back() == ' ') parts.pop_back();
                    resp += parts;
                    if (!literal.empty() || want_full || want_hdr || want_text)
                        resp += " {" + std::to_string(literal.size()) + "}\r\n" + literal;
                    resp += ")\r\n";
                    if (!send_all(fd, resp)) break;
                }
                send_all(fd, tag + " OK FETCH completed\r\n");
            }
            // ── M4a: STORE — change flags (mark as read etc.) ─────────────
            else if (cmd == "STORE") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO SELECT required first\r\n"); continue; }
                // args: "<seq-set> <op>FLAGS[.SILENT] (<flags>)"
                size_t sp = args.find(' ');
                if (sp == std::string::npos) { send_all(fd, tag + " BAD STORE argument\r\n"); continue; }
                std::string seqset = args.substr(0, sp);
                std::string rest   = args.substr(sp + 1);
                std::string rest_u = rest; for (char& c : rest_u) c = (char)std::toupper((unsigned char)c);
                char op = '=';
                if (!rest_u.empty() && rest_u[0] == '+') op = '+';
                else if (!rest_u.empty() && rest_u[0] == '-') op = '-';
                bool silent = rest_u.find(".SILENT") != std::string::npos;
                // flag list inside parentheses
                size_t lp = rest.find('('), rp = rest.rfind(')');
                std::string flaglist = (lp != std::string::npos && rp != std::string::npos && rp > lp)
                                       ? rest.substr(lp + 1, rp - lp - 1) : "";
                // collect the requested Maildir letters
                std::string want; { std::istringstream fs2(flaglist); std::string f;
                    while (fs2 >> f) { char m = imap_to_maildir_flag(f); if (m) want += m; } }

                auto& msgs = messages;   // stable snapshot
                size_t lo = 0, hi = 0;
                if (!parse_seqset(seqset, msgs.size(), lo, hi)) { send_all(fd, tag + " OK STORE completed (empty)\r\n"); continue; }

                std::string out;
                for (size_t i = lo; i <= hi; i++) {
                    std::string cur = maildir_flags(msgs[i-1]);
                    std::string nf;
                    if (op == '=') nf = want;
                    else if (op == '+') { nf = cur; for (char c : want) if (nf.find(c)==std::string::npos) nf += c; }
                    else { nf = cur; for (char c : want) { size_t k = nf.find(c); if (k!=std::string::npos) nf.erase(k,1); } }
                    std::string np = apply_flags(msgs[i-1], nf);
                    if (np.empty()) continue;  // concurrent change -> skip
                    msgs[i-1] = np;            // update the path at the same index (SEQ STAYS FIXED)
                    if (!silent)
                        out += "* " + std::to_string(i) + " FETCH (FLAGS " + maildir_to_imap_flags(normalize_flags(nf)) + ")\r\n";
                }
                out += tag + " OK STORE completed\r\n";
                send_all(fd, out);
            }
            // ── M4a: EXPUNGE — permanently delete messages marked \Deleted ─────────
            else if (cmd == "EXPUNGE") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO SELECT required first\r\n"); continue; }
                // RFC 3501: EXPUNGE responses in DESCENDING sequence order; delete from the snapshot
                // in descending order too so that the indexes shift consistently (sequence changes only
                // HERE — the stability rule).
                std::string out;
                std::error_code ec;
                for (size_t i = messages.size(); i >= 1; i--) {
                    if (maildir_flags(messages[i-1]).find('T') != std::string::npos) {
                        fs::remove(messages[i-1], ec);
                        if (!ec) {
                            messages.erase(messages.begin() + (i - 1));  // remove from the snapshot
                            out += "* " + std::to_string(i) + " EXPUNGE\r\n";
                        }
                    }
                }
                out += tag + " OK EXPUNGE completed\r\n";
                send_all(fd, out);
            }
            // ── M4b: APPEND — add a message (save to Sent, upload a draft) ───────
            // SECURITY: literal {N} is client-controlled -> MAX_LITERAL cap (OOM),
            // guarded parse, path-traversal (resolve_mailbox), atomic write.
            else if (cmd == "APPEND") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                // args: "<mailbox> [(<flags>)] [\"date\"] {<size>}"
                // Literal size: last '{' ... '}' (optional '+' non-sync)
                size_t lb = args.rfind('{'), rb = args.rfind('}');
                if (lb == std::string::npos || rb == std::string::npos || rb < lb) {
                    send_all(fd, tag + " BAD APPEND literal {N} required\r\n"); continue;
                }
                std::string szs = args.substr(lb + 1, rb - lb - 1);
                bool nonsync = !szs.empty() && szs.back() == '+';
                if (nonsync) szs.pop_back();
                size_t litsize = 0;
                if (szs.empty() || szs.find_first_not_of("0123456789") != std::string::npos) {
                    send_all(fd, tag + " BAD APPEND invalid literal size\r\n"); continue;
                }
                try { litsize = std::stoull(szs); }
                catch (...) { send_all(fd, tag + " BAD APPEND literal size\r\n"); continue; }
                // OOM protection — if the cap is exceeded: reject WITHOUT reading the data at all
                if (litsize > imap_max_literal()) {
                    send_all(fd, tag + " NO [TOOBIG] APPEND message too large\r\n"); continue;
                }
                // mailbox name (first token)
                std::string head = args.substr(0, lb);
                std::string mbname; { std::istringstream hs(head); hs >> mbname; }
                std::string box = resolve_mailbox(maildir_root, mbname);
                if (box.empty()) { send_all(fd, tag + " NO [TRYCREATE] invalid mailbox\r\n"); continue; }
                // flags (inside parentheses, optional)
                std::string mflags;
                size_t fp = head.find('(');
                if (fp != std::string::npos) {
                    size_t fpe = head.find(')', fp);
                    if (fpe != std::string::npos) {
                        std::istringstream fs2(head.substr(fp + 1, fpe - fp - 1)); std::string f;
                        while (fs2 >> f) { char m = imap_to_maildir_flag(f); if (m) mflags += m; }
                    }
                }
                // Synchronizing literal: send a continuation request
                if (!nonsync) { if (!send_all(fd, "+ literal data expected\r\n")) break; }
                // Read exactly litsize bytes (bounded — the cap was already validated)
                std::string data;
                if (!read_exact(fd, litsize, data)) break;  // connection/timeout
                // swallow the line remainder (CRLF) after the literal
                std::string tail; read_line(fd, tail);
                // Write atomically
                if (maildir_append(box, mflags, data))
                    send_all(fd, tag + " OK [APPENDUID 1 1] APPEND completed\r\n");
                else
                    send_all(fd, tag + " NO APPEND could not write\r\n");
            }
            // ── M4c: SEARCH — return the sequence numbers of messages matching the criteria ──
            // UID FETCH/STORE/SEARCH/COPY — required by RFC 3501 §6.4.8 but NOT in M1.
            // It cannot be implemented without a persistent UID (see the UID rejection in FETCH).
            // State the REASON instead of `BAD unknown command`: a developer debugging
            // Thunderbird should not search for "no command, or syntax error".
            else if (cmd == "UID") {
                send_all(fd, tag + " NO [CANNOT] UID command not supported — no persistent UID "
                                   "(Milestone 1). Use sequence-based FETCH/STORE/SEARCH.\r\n");
            }
            // CREATE/DELETE/RENAME are not in M1 scope either — state the scope instead
            // of a silent "unknown command".
            else if (cmd == "CREATE" || cmd == "DELETE" || cmd == "RENAME" ||
                     cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") {
                send_all(fd, tag + " NO [CANNOT] " + cmd + " not supported (Milestone 1: "
                                   "only the current INBOX is read).\r\n");
            }
            else if (cmd == "SEARCH") {
                if (!authenticated) { send_all(fd, tag + " NO LOGIN required first\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO SELECT required first\r\n"); continue; }

                // Simple quote-aware tokenizer (SEARCH FROM "a b" ...)
                std::vector<std::string> tok;
                for (size_t p = 0; p < args.size();) {
                    while (p < args.size() && args[p] == ' ') p++;
                    if (p >= args.size()) break;
                    std::string t;
                    if (args[p] == '"') { p++; while (p < args.size() && args[p] != '"') t += args[p++]; if (p < args.size()) p++; }
                    else { while (p < args.size() && args[p] != ' ') t += args[p++]; }
                    tok.push_back(t);
                }
                auto lc = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
                auto up = [](const std::string& s) { std::string r; for (char c : s) r += (char)std::toupper((unsigned char)c); return r; };

                // Parse the criteria (all ANDed). Bad syntax -> BAD.
                struct Crit { std::string kw, a1, a2; size_t lo = 0, hi = 0; };
                std::vector<Crit> crits;
                bool bad = false, needs_content = false;
                for (size_t k = 0; k < tok.size(); k++) {
                    std::string K = up(tok[k]);
                    if (K == "CHARSET") { k++; continue; }           // CHARSET <name> — skip
                    if (K == "ALL" || K == "RECENT" || K == "NEW" || K == "OLD" ||
                        K == "SEEN" || K == "UNSEEN" || K == "DELETED" || K == "UNDELETED" ||
                        K == "FLAGGED" || K == "UNFLAGGED" || K == "ANSWERED" || K == "UNANSWERED" ||
                        K == "DRAFT" || K == "UNDRAFT") {
                        crits.push_back({K, "", ""});
                    }
                    else if (K == "FROM" || K == "TO" || K == "CC" || K == "SUBJECT" ||
                             K == "BODY" || K == "TEXT") {
                        if (k + 1 >= tok.size()) { bad = true; break; }
                        crits.push_back({K, lc(tok[++k]), ""}); needs_content = true;
                    }
                    else if (K == "HEADER") {
                        if (k + 2 >= tok.size()) { bad = true; break; }
                        crits.push_back({K, lc(tok[k + 1]), lc(tok[k + 2])}); k += 2; needs_content = true;
                    }
                    else if (K == "UID" || (!tok[k].empty() && (std::isdigit((unsigned char)tok[k][0]) || tok[k][0] == '*'))) {
                        std::string set = (K == "UID") ? (k + 1 < tok.size() ? tok[++k] : "") : tok[k];
                        Crit c; c.kw = "SEQ";
                        if (!parse_seqset(set, messages.size(), c.lo, c.hi)) { c.lo = 1; c.hi = 0; } // does not match
                        crits.push_back(c);
                    }
                    else { bad = true; break; }
                }
                if (bad) { send_all(fd, tag + " BAD SEARCH criteria could not be parsed\r\n"); continue; }

                auto hdr_field = [&](const std::string& hdr, const std::string& field) {
                    // concatenate the values (lower-cased) of the lines starting with "field:"
                    std::string want = field + ":", out; size_t i = 0;
                    while (i < hdr.size()) {
                        size_t e = hdr.find('\n', i); std::string line = hdr.substr(i, e == std::string::npos ? std::string::npos : e - i);
                        if (line.size() >= want.size() && lc(line.substr(0, want.size())) == want)
                            out += lc(line.substr(want.size())) + "\n";
                        if (e == std::string::npos) break; i = e + 1;
                    }
                    return out;
                };

                std::string result = "* SEARCH";
                for (size_t i = 1; i <= messages.size(); i++) {
                    const std::string& path = messages[i - 1];
                    std::string fl = maildir_flags(path);
                    bool recent = path.find("/new/") != std::string::npos;
                    std::string raw, hdr, body;
                    if (needs_content) {
                        if (!read_file_capped(path, raw)) continue;
                        split_header_body(raw, hdr, body);
                    }
                    bool ok = true;
                    for (const auto& c : crits) {
                        bool m = true;
                        if      (c.kw == "ALL")       m = true;
                        else if (c.kw == "RECENT" || c.kw == "NEW") m = recent && (c.kw == "RECENT" || fl.find('S') == std::string::npos);
                        else if (c.kw == "OLD")       m = !recent;
                        else if (c.kw == "SEEN")      m = fl.find('S') != std::string::npos;
                        else if (c.kw == "UNSEEN")    m = fl.find('S') == std::string::npos;
                        else if (c.kw == "DELETED")   m = fl.find('T') != std::string::npos;
                        else if (c.kw == "UNDELETED") m = fl.find('T') == std::string::npos;
                        else if (c.kw == "FLAGGED")   m = fl.find('F') != std::string::npos;
                        else if (c.kw == "UNFLAGGED") m = fl.find('F') == std::string::npos;
                        else if (c.kw == "ANSWERED")  m = fl.find('R') != std::string::npos;
                        else if (c.kw == "UNANSWERED")m = fl.find('R') == std::string::npos;
                        else if (c.kw == "DRAFT")     m = fl.find('D') != std::string::npos;
                        else if (c.kw == "UNDRAFT")   m = fl.find('D') == std::string::npos;
                        else if (c.kw == "SEQ")       m = (i >= c.lo && i <= c.hi);
                        else if (c.kw == "FROM")      m = hdr_field(hdr, "from").find(c.a1) != std::string::npos;
                        else if (c.kw == "TO")        m = hdr_field(hdr, "to").find(c.a1) != std::string::npos;
                        else if (c.kw == "CC")        m = hdr_field(hdr, "cc").find(c.a1) != std::string::npos;
                        else if (c.kw == "SUBJECT")   m = hdr_field(hdr, "subject").find(c.a1) != std::string::npos;
                        else if (c.kw == "BODY")      m = lc(body).find(c.a1) != std::string::npos;
                        else if (c.kw == "TEXT")      m = lc(raw).find(c.a1) != std::string::npos;
                        else if (c.kw == "HEADER")    m = hdr_field(hdr, c.a1).find(c.a2) != std::string::npos;
                        if (!m) { ok = false; break; }
                    }
                    if (ok) result += " " + std::to_string(i);
                }
                send_all(fd, result + "\r\n" + tag + " OK SEARCH completed\r\n");
            }
            else {
                send_all(fd, tag + " BAD unknown command\r\n");
                if (++errors >= imap_max_errors()) break;
            }
        }
    }

    void accept_loop(int lfd, bool implicit_tls) {
        while (running.load()) {
            int cfd = (int)::accept(lfd, nullptr, nullptr);
            if (cfd < 0) { if (!running.load()) break; continue; }
            // Concurrent connection limit (DoS)
            if (conn_count.load() >= imap_max_conn()) {
                // Note: on the implicit-TLS port we cannot send a plaintext BYE
                //  before the handshake; just close.
                if (!implicit_tls) send_all(cfd, "* BYE too many connections, try again later\r\n");
                close(cfd);
                continue;
            }
            conn_count++;
            std::thread([this, cfd, implicit_tls]() { run_session(cfd, implicit_tls); }).detach();
        }
    }
};

// ── Public API ────────────────────────────────────────────────────────────────
ImapServer::ImapServer(int port_imap, int port_imaps, int workers, ImapAuthHandler auth)
    : impl_(std::make_unique<Impl>())
{
    impl_->port_imap  = port_imap;
    impl_->port_imaps = port_imaps;
    impl_->workers    = workers;
    impl_->auth       = std::move(auth);
}

ImapServer::~ImapServer() { stop(); }

bool ImapServer::start() {
    impl_->listen_fd = Impl::make_server_fd(impl_->port_imap);
    if (impl_->listen_fd < 0) {
        Logger::instance().log(LogLevel::LOG_ERROR, "IMAP",
            "cannot bind port " + std::to_string(impl_->port_imap));
        return false;
    }
    impl_->ssl_ctx = Impl::make_ssl_ctx();   // null = no certificate env (plaintext dev)
    impl_->running = true;
    // The descriptor goes to the thread BY VALUE: stop() sets listen_fd to -1 after it closes it, and a thread
    // that reads the member when it starts would race with that (ThreadSanitizer: data race).
    { const int lfd = impl_->listen_fd;
      impl_->accept_thread = std::thread([this, lfd]() { impl_->accept_loop(lfd, false); }); }

    // IMAPS (implicit TLS) — only if certificate + port are present.
    if (impl_->ssl_ctx && impl_->port_imaps > 0) {
        impl_->listen_fd_tls = Impl::make_server_fd(impl_->port_imaps);
        if (impl_->listen_fd_tls >= 0) {
            const int tfd = impl_->listen_fd_tls;
            impl_->accept_thread_tls = std::thread([this, tfd]() { impl_->accept_loop(tfd, true); });
            Logger::instance().log(LogLevel::LOG_INFO, "IMAP",
                "IMAPS (implicit TLS) listening on port " + std::to_string(impl_->port_imaps));
        }
    }
    Logger::instance().log(LogLevel::LOG_INFO, "IMAP",
        std::string("IMAP4rev1 listening on port ") + std::to_string(impl_->port_imap) +
        (impl_->ssl_ctx ? " (STARTTLS enabled)" : " (TLS not configured — plaintext)"));
    return true;
}

void ImapServer::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->listen_fd >= 0)     { close(impl_->listen_fd);     impl_->listen_fd = -1; }
    if (impl_->listen_fd_tls >= 0) { close(impl_->listen_fd_tls); impl_->listen_fd_tls = -1; }
    if (impl_->accept_thread.joinable())     impl_->accept_thread.join();
    if (impl_->accept_thread_tls.joinable()) impl_->accept_thread_tls.join();
    if (impl_->ssl_ctx) { SSL_CTX_free(impl_->ssl_ctx); impl_->ssl_ctx = nullptr; }
}

} // namespace look

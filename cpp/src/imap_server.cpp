// LOOK IMAP4rev1 Server (RFC 3501) — Milestone 1
//
// M1 kapsamı: greeting · CAPABILITY · LOGIN · LOGOUT · NOOP.
// Model: thread-per-connection (blocking), MAX_CONN ile sınırlı. IMAP oturumları
// uzun ömürlüdür; bu model doğrulaması kolay ve IDLE (M6) için doğal zemindir.
//
// GÜVENLİK (bu oturumun dersleri baştan uygulandı):
//   • Satır uzunluğu cap (RESP2 read_line → OOM dersi)
//   • Literal {N} boyut cap (RESP2 read_bulk → OOM dersi)
//   • std::stoul guarded (protokol parser stoi crash dersi)
//   • Başarısız login gecikmesi (brute-force yavaşlatma)
//   • Eşzamanlı bağlantı + oturum-başına hata sınırı (DoS)

#include "look/imap_server.h"
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

// ── TLS: aktif bağlantının SSL nesnesi (thread-per-connection → thread_local) ──
// Her oturum kendi thread'inde çalışır; I/O yardımcıları bu işaretçiyi kontrol
// ederek ::send/::recv yerine SSL_write/SSL_read kullanır. Böylece 40+ çağrı
// noktası değişmeden TLS-farkında olur.
static thread_local SSL* g_tls = nullptr;

// ── Env yapılandırma (güvenlik limitleri) ─────────────────────────────────────
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
    int              workers;   // M1'de kullanılmıyor (thread-per-conn); M6+ için
    ImapAuthHandler  auth;

    int                 listen_fd = -1;      // 143 (STARTTLS)
    int                 listen_fd_tls = -1;  // 993 (implicit TLS / IMAPS)
    std::atomic<bool>   running{false};
    std::atomic<size_t> conn_count{0};
    std::thread         accept_thread;
    std::thread         accept_thread_tls;
    SSL_CTX*            ssl_ctx = nullptr;   // null = TLS yapılandırılmamış

    // ── TLS sunucu context'i (SMTP ile aynı desen; sertifika env'den) ─────────
    // LOOK_IMAP_CERT/LOOK_IMAP_KEY, yoksa LOOK_SMTP_CERT/LOOK_SMTP_KEY paylaşılır.
    static SSL_CTX* make_ssl_ctx() {
        const char* cert = std::getenv("LOOK_IMAP_CERT");
        const char* key  = std::getenv("LOOK_IMAP_KEY");
        if (!cert || !key) { cert = std::getenv("LOOK_SMTP_CERT"); key = std::getenv("LOOK_SMTP_KEY"); }
        if (!cert || !key || !*cert || !*key) return nullptr;   // TLS yapılandırılmamış
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) return nullptr;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);     // TLS 1.2 taban
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                  SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            Logger::instance().log(LogLevel::LOG_ERROR, "IMAP",
                "TLS: sertifika/anahtar yüklenemedi — TLS devre dışı");
            SSL_CTX_free(ctx);
            return nullptr;
        }
        return ctx;
    }

    // TLS el sıkışması: fd üzerinde SSL_accept, başarılıysa g_tls ayarlanır.
    bool tls_handshake(int fd) {
        if (!ssl_ctx) return false;
        SSL* ssl = SSL_new(ssl_ctx);
        if (!ssl) return false;
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) { SSL_free(ssl); return false; }
        g_tls = ssl;
        return true;
    }

    // ── Güvenli dinleme soketi (SMTP ile aynı desen: dual-stack IPv6) ─────────
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
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no));  // IPv4+IPv6
        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons((uint16_t)port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
        if (::listen(fd, 128) < 0) { close(fd); return -1; }
        return fd;
    }

    // ── Ham I/O ──────────────────────────────────────────────────────────────
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

    // Soket ms milisaniye içinde okunabilir mi? (IDLE'da hem DONE hem yeni-mail
    // izlemek için). select() — Winsock+POSIX ortak. TLS'te tampondaki veri de
    // "okunabilir" sayılır (SSL_pending). Dönen: 1=hazır, 0=zaman aşımı, -1=hata.
    static int wait_readable(int fd, int ms) {
        if (g_tls && SSL_pending(g_tls) > 0) return 1;
        fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
        struct timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        int r = ::select(fd + 1, &rs, nullptr, nullptr, &tv);
        return r;
    }

    // TLS'e duyarlı tek-blok okuma (::recv veya SSL_read).
    static ssize_t conn_recv(int fd, char* buf, size_t len) {
        if (g_tls) return SSL_read(g_tls, buf, (int)len);
#if defined(_WIN32)
        return ::recv(fd, buf, (int)len, 0);
#else
        return ::recv(fd, buf, len, 0);
#endif
    }

    // Slowloris koruması: idle recv timeout. Saldırgan bağlantı açıp hiç/çok
    // yavaş veri gönderemesin — thread süresiz bloklanmaz. LOOK_IMAP_IDLE_TIMEOUT
    // (sn, default 1800 = 30 dk, RFC autologout önerisi). MAX_CONN ile birlikte
    // toplam kaynak sınırlı.
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

    // Tam olarak n byte oku (literal veri için). Bounded — n çağıran tarafça
    // MAX_LITERAL'e karşı doğrulanmış olmalı. false → bağlantı/timeout.
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

    // Maildir'e atomik yaz: tmp/<unique> → new/<unique>[:2,<flags>] rename.
    // Crash-safe (kısmi mesaj new/'de görünmez). Benzersiz ad: epoch.sayaç.rastgele.
    // Dönen: false = başarısız.
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
        // Flag varsa doğrudan cur/'a (okunmuş/işaretli), yoksa new/'e (RFC Maildir)
        fs::path dest = nf.empty() ? (fs::path(box) / "new" / unique)
                                   : (fs::path(box) / "cur" / (unique + ":2," + nf));
        fs::rename(tmp, dest, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    }

    // Bir CRLF-sonlu komut satırı oku. Satır MAX_LINE ile sınırlı.
    // false → bağlantı kapandı veya limit aşıldı.
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
            if (out.size() > max_line) return false;  // satır bombası → kes
        }
    }

    // "tag SP command SP args" ayrıştır. tag ve command döner, kalan args.
    static void parse_command(const std::string& line, std::string& tag,
                              std::string& cmd, std::string& args) {
        tag.clear(); cmd.clear(); args.clear();
        size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos) { tag = line; return; }
        tag = line.substr(0, sp1);
        size_t sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) { cmd = line.substr(sp1 + 1); }
        else { cmd = line.substr(sp1 + 1, sp2 - sp1 - 1); args = line.substr(sp2 + 1); }
        for (char& ch : cmd) ch = (char)std::toupper((unsigned char)ch);  // komut case-insensitive
    }

    // İki tırnak-içi argümanı çıkar (LOGIN "user" "pass" veya LOGIN user pass).
    static void split_two(const std::string& args, std::string& a, std::string& b) {
        a.clear(); b.clear();
        auto unquote = [](std::string s) {
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
            return s;
        };
        // basit: tırnaklıysa tırnak-sınırlarına, değilse boşluğa göre böl
        size_t i = 0;
        auto next_token = [&](std::string& tok) {
            while (i < args.size() && args[i] == ' ') i++;
            if (i >= args.size()) return false;
            if (args[i] == '"') {
                size_t e = args.find('"', i + 1);
                if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
                else { tok = args.substr(i, e - i + 1); i = e + 1; }
            } else {
                size_t e = args.find(' ', i);
                if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
                else { tok = args.substr(i, e - i); i = e; }
            }
            return true;
        };
        std::string t1, t2;
        if (next_token(t1)) a = unquote(t1);
        if (next_token(t2)) b = unquote(t2);
    }

    // ── GÜVENLİK: mailbox adı → Maildir yolu (path traversal koruması) ───────
    // Kullanıcı "SELECT ../../etc" veya "SELECT /etc/passwd" diyemez.
    // INBOX → kullanıcının kök Maildir'i; diğerleri kök altında canonical
    // olarak doğrulanır. Kök dışına çıkan her isim reddedilir.
    // Dönen: geçerli yol; boş = geçersiz/traversal.
    static std::string resolve_mailbox(const std::string& root, const std::string& name) {
        if (root.empty()) return "";
        // Tırnakları temizle
        std::string mb = name;
        if (mb.size() >= 2 && mb.front() == '"' && mb.back() == '"') mb = mb.substr(1, mb.size() - 2);
        if (mb.empty() || mb == "INBOX") return root;   // INBOX = kök Maildir

        // Tehlikeli bileşenleri baştan ele — netlik + defense-in-depth:
        // mutlak yol, ".." segmenti, null byte. (Canonical check zaten kökü
        // koruyor; bu, geçersiz ismi net "NO" ile reddeder.)
        if (mb.find('\0') != std::string::npos) return "";
        if (mb.front() == '/' || mb.front() == '\\') return "";
        if (mb.find("..") != std::string::npos) return "";

        std::error_code ec;
        fs::path root_c = fs::weakly_canonical(fs::path(root), ec);
        if (ec) return "";
        // Maildir++ alt klasör: "Sent" → root/.Sent  (nokta prefix)
        fs::path target = fs::path(root) / ("." + mb);
        fs::path target_c = fs::weakly_canonical(target, ec);
        if (ec) return "";
        // target_c kesinlikle root_c altında olmalı (traversal kilidi). Düz
        // string-prefix YETMEZ ("/root" öneki "/root-evil" ile eşleşir); ayırıcı-
        // sınırı da zorunlu — üstteki ".." reddine bağlı kalmayan, kendi başına
        // doğru containment (installer/template ile aynı disiplin).
        auto rs = root_c.string(), ts = target_c.string();
        bool within = ts.size() >= rs.size() && ts.compare(0, rs.size(), rs) == 0 &&
                      (ts.size() == rs.size() || ts[rs.size()] == '/' || ts[rs.size()] == '\\');
        if (!within) return "";
        return target_c.string();
    }

    // Maildir'de mesaj say: new/ + cur/ altındaki dosyalar. recent = new/.
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

    // Kullanıcının mailbox'larını listele: kök = INBOX, .X klasörleri = alt kutular.
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

    // Mailbox'taki mesaj dosyalarını deterministik sırayla topla (kararlı
    // sequence numarası için). new/ ve cur/ birleşik, dosya adına göre sıralı.
    static std::vector<std::string> build_messages(const std::string& box) {
        std::vector<std::string> files;
        std::error_code ec;
        for (const char* sub : {"cur", "new"}) {   // cur önce (okunmuşlar), sonra new
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

    // Dosyayı belleğe oku (boyut MAX_LITERAL ile sınırlı — dev mesaj OOM koruması)
    static bool read_file_capped(const std::string& path, std::string& out) {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec || sz > imap_max_literal()) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return true;
    }

    // RFC 5322: header ve body'yi ayır (\r\n\r\n veya \n\n).
    static void split_header_body(const std::string& msg, std::string& hdr, std::string& body) {
        size_t p = msg.find("\r\n\r\n");
        size_t skip = 4;
        if (p == std::string::npos) { p = msg.find("\n\n"); skip = 2; }
        if (p == std::string::npos) { hdr = msg; body.clear(); return; }
        hdr  = msg.substr(0, p + (skip == 4 ? 2 : 1));  // header sonu CRLF dahil
        body = msg.substr(p + skip);
    }

    // seq-set ayrıştır ("1", "1:3", "1:*", "*") → [lo, hi] (1-tabanlı, kapsayıcı).
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

    // ── Maildir flag kodlaması (RFC + Maildir standardı) ─────────────────────
    // Maildir dosya adı: "<base>:2,<flags>" — flags BÜYÜK harf, ALFABETİK sıralı.
    // IMAP → Maildir: \Draft=D \Flagged=F \Answered=R \Seen=S \Deleted=T
    // new/ altındaki mesajın flag'i yoktur; flag verilince cur/'a taşınır.

    // Dosya adından mevcut Maildir flag setini çıkar (":2,XYZ" → "XYZ").
    static std::string maildir_flags(const std::string& path) {
        std::string name = fs::path(path).filename().string();
        size_t p = name.find(":2,");
        if (p == std::string::npos) return "";
        std::string f = name.substr(p + 3);
        // info kısmı yalnızca flag harfleri — güvenli tarafta sadece bilinenleri al
        std::string out;
        for (char c : f) if (std::strchr("DFPRST", c)) out += c;
        return out;
    }

    // IMAP flag adı → Maildir harfi (bilinmeyen → 0).
    static char imap_to_maildir_flag(const std::string& f) {
        std::string u = f; for (char& c : u) c = (char)std::toupper((unsigned char)c);
        if (u == "\\SEEN")     return 'S';
        if (u == "\\ANSWERED") return 'R';
        if (u == "\\FLAGGED")  return 'F';
        if (u == "\\DELETED")  return 'T';
        if (u == "\\DRAFT")    return 'D';
        return 0;  // \Recent ve custom flag'ler Maildir'de saklanmaz
    }

    // Maildir harf setini alfabetik, tekilleştirilmiş stringe çevir (Maildir kuralı).
    static std::string normalize_flags(const std::string& in) {
        bool seen[256] = {false};
        for (unsigned char c : in) if (std::strchr("DFPRST", c)) seen[c] = true;
        std::string out;
        for (char c : std::string("DFPRST")) if (seen[(unsigned char)c]) out += c;
        return out;
    }

    // Maildir flag setini IMAP FLAGS gösterimine çevir ("(\Seen \Deleted)").
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

    // Dosyayı yeni flag setiyle yeniden adlandır (gerekirse new/ → cur/).
    // Dönen: yeni yol; boş = başarısız (eşzamanlı silme/taşıma → sessiz atla).
    static std::string apply_flags(const std::string& path, const std::string& new_flags) {
        std::error_code ec;
        fs::path pp(path);
        std::string name = pp.filename().string();
        std::string base = name;
        size_t p = name.find(":2,");
        if (p != std::string::npos) base = name.substr(0, p);
        std::string nf = normalize_flags(new_flags);
        std::string newname = base + ":2," + nf;
        // Hedef klasör: flag verildiyse cur/, aksi halde bulunduğu yer.
        fs::path parent = pp.parent_path();
        bool in_new = parent.filename() == "new";
        fs::path dest_dir = in_new ? (parent.parent_path() / "cur") : parent;
        fs::path dest = dest_dir / newname;
        if (dest == pp) return path;  // değişiklik yok
        fs::rename(pp, dest, ec);     // aynı FS'te atomik
        if (ec) return "";            // kaynak gitmiş (eşzamanlı) → atla
        return dest.string();
    }

    // ── Oturum döngüsü (bir bağlantı = bir thread) ───────────────────────────
    void run_session(int fd, bool implicit_tls) {
        struct ConnGuard {
            std::atomic<size_t>& c; int fd;
            ~ConnGuard() {
                if (g_tls) { SSL_shutdown(g_tls); SSL_free(g_tls); g_tls = nullptr; }
                c--; close(fd);
            }
        } guard{conn_count, fd};

        set_recv_timeout(fd);   // Slowloris koruması

        // IMAPS (993): bağlantı açılır açılmaz TLS el sıkışması (implicit TLS).
        if (implicit_tls) {
            if (!tls_handshake(fd)) return;   // el sıkışma başarısız → düş
        }

        // TLS aktifken CAPABILITY'de STARTTLS reklamı yapılmaz; değilse yapılır.
        auto cap_line = [&]() -> std::string {
            std::string c = "IMAP4rev1 IDLE";
            if (ssl_ctx && !g_tls) c += " STARTTLS LOGINDISABLED";
            return c;
        };

        // Greeting
        if (!send_all(fd, "* OK [CAPABILITY " + cap_line() + "] LOOK IMAP hazır\r\n")) return;

        bool authenticated = false;
        std::string maildir_root;      // LOGIN sonrası kullanıcının Maildir kökü
        std::string selected;          // seçili mailbox yolu ("" = seçili yok)
        // KARARLI mesaj snapshot'ı — SELECT'te alınır. Sequence numaraları buna
        // indeksler (RFC 3501): oturum içinde SABİT, yalnız EXPUNGE değiştirir.
        // STORE dosyayı rename edince aynı indekste yol güncellenir (re-sort YOK).
        std::vector<std::string> messages;
        int  errors = 0;
        std::string line, tag, cmd, args;

        while (running.load()) {
            if (!read_line(fd, line)) break;
            parse_command(line, tag, cmd, args);
            if (tag.empty()) { send_all(fd, "* BAD boş komut\r\n"); if (++errors >= imap_max_errors()) break; continue; }

            if (cmd == "CAPABILITY") {
                send_all(fd, "* CAPABILITY " + cap_line() + "\r\n" + tag + " OK CAPABILITY tamamlandı\r\n");
            }
            else if (cmd == "STARTTLS") {
                if (g_tls) { send_all(fd, tag + " NO TLS zaten aktif\r\n"); continue; }
                if (!ssl_ctx) { send_all(fd, tag + " NO STARTTLS yapılandırılmamış\r\n"); continue; }
                // Önce OK gönder (plaintext), sonra el sıkış (RFC 3501 6.2.1).
                if (!send_all(fd, tag + " OK TLS el sıkışmasına başlanıyor\r\n")) break;
                if (!tls_handshake(fd)) break;   // başarısız → bağlantıyı düşür
                // Güvenlik (RFC 3501): TLS öncesi durum çöpe atılır — kimlik sıfırla.
                authenticated = false; maildir_root.clear(); selected.clear(); messages.clear();
            }
            else if (cmd == "NOOP") {
                send_all(fd, tag + " OK NOOP tamamlandı\r\n");
            }
            // ── M6: IDLE — yeni mail için canlı push (RFC 2177) ──────────────
            else if (cmd == "IDLE") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO önce SELECT gerekli\r\n"); continue; }
                if (!send_all(fd, "+ idling\r\n")) break;
                // Sequence KARARLILIĞI: yeni mesajlar snapshot'a EKLENİR (mevcut
                // indeksler değişmez). new/ tarayıp henüz bilinmeyenleri sona ekle.
                bool broke = false;
                int tick_ms = (int)env_long("LOOK_IMAP_IDLE_TICK_MS", 15000);
                while (running.load()) {
                    int rr = wait_readable(fd, tick_ms);   // periyodik mailbox taraması
                    if (rr < 0) { broke = true; break; }  // select hatası → oturum düş
                    if (rr == 0) {
                        // Zaman aşımı → mailbox'ta yeni mesaj var mı?
                        std::vector<std::string> fresh = build_messages(selected);
                        std::vector<std::string> added;
                        for (auto& p : fresh)
                            if (std::find(messages.begin(), messages.end(), p) == messages.end())
                                added.push_back(p);
                        if (!added.empty()) {
                            for (auto& p : added) messages.push_back(p);  // sona ekle → seq sabit
                            size_t recent = 0;
                            for (auto& m : messages) if (m.find("/new/") != std::string::npos) recent++;
                            if (!send_all(fd, "* " + std::to_string(messages.size()) + " EXISTS\r\n"
                                              "* " + std::to_string(recent) + " RECENT\r\n")) { broke = true; break; }
                        }
                        continue;
                    }
                    // Okunabilir → istemciden satır (yalnız "DONE" beklenir)
                    std::string dline;
                    if (!read_line(fd, dline)) { broke = true; break; }
                    std::string du = dline;
                    for (char& c : du) c = (char)std::toupper((unsigned char)c);
                    if (du == "DONE") break;
                    // RFC: IDLE içinde DONE dışında komut yok → protokol hatası
                    if (!send_all(fd, tag + " BAD IDLE'ı bitirmek için DONE gerekli\r\n")) { broke = true; break; }
                    broke = true; break;
                }
                if (broke) break;
                send_all(fd, tag + " OK IDLE tamamlandı\r\n");
            }
            else if (cmd == "LOGOUT") {
                send_all(fd, "* BYE LOOK IMAP oturum kapanıyor\r\n" + tag + " OK LOGOUT tamamlandı\r\n");
                break;
            }
            else if (cmd == "LOGIN") {
                // Güvenlik: TLS varsa düz-metin LOGIN reddedilir (LOGINDISABLED).
                if (ssl_ctx && !g_tls) {
                    send_all(fd, tag + " NO [PRIVACYREQUIRED] önce STARTTLS gerekli\r\n");
                    continue;
                }
                std::string user, pass;
                split_two(args, user, pass);
                ImapAuthResult r = auth ? auth(user, pass) : ImapAuthResult{};
                if (r.ok) {
                    authenticated = true;
                    maildir_root  = r.maildir_path;
                    send_all(fd, tag + " OK LOGIN başarılı\r\n");
                } else {
                    // Brute-force yavaşlatma — başarısız denemede sabit gecikme
                    std::this_thread::sleep_for(std::chrono::milliseconds(imap_auth_delay_ms()));
                    send_all(fd, tag + " NO LOGIN başarısız\r\n");
                    if (++errors >= imap_max_errors()) break;
                }
            }
            // ── M2: SELECT / EXAMINE — mailbox seç, mesaj say ────────────────
            else if (cmd == "SELECT" || cmd == "EXAMINE") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                std::string box = resolve_mailbox(maildir_root, args);
                if (box.empty()) { send_all(fd, tag + " NO geçersiz mailbox adı\r\n"); continue; }
                // Kararlı snapshot al — sequence numaraları bu andan itibaren sabit
                selected = box;
                messages = build_messages(box);
                size_t total = messages.size();
                size_t recent = 0;
                for (auto& m : messages) if (m.find("/new/") != std::string::npos) recent++;
                std::string ro = (cmd == "EXAMINE") ? "READ-ONLY" : "READ-WRITE";
                std::string resp =
                    "* " + std::to_string(total)  + " EXISTS\r\n"
                    "* " + std::to_string(recent) + " RECENT\r\n"
                    "* OK [UIDVALIDITY 1] UID geçerlilik\r\n"
                    "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
                    "* OK [PERMANENTFLAGS (\\Seen \\Deleted)] kalıcı bayraklar\r\n"
                    + tag + " OK [" + ro + "] " + cmd + " tamamlandı\r\n";
                send_all(fd, resp);
            }
            // ── M2: LIST — mailbox'ları listele ──────────────────────────────
            else if (cmd == "LIST" || cmd == "LSUB") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                std::string resp;
                for (auto& b : list_mailboxes(maildir_root))
                    resp += "* " + cmd + " (\\HasNoChildren) \"/\" \"" + b + "\"\r\n";
                resp += tag + " OK " + cmd + " tamamlandı\r\n";
                send_all(fd, resp);
            }
            // ── M2: STATUS — mailbox durumu ──────────────────────────────────
            else if (cmd == "STATUS") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                std::string mbname = args;
                size_t sp = mbname.find(' ');
                if (sp != std::string::npos) mbname = mbname.substr(0, sp);  // ilk arg = mailbox
                std::string box = resolve_mailbox(maildir_root, mbname);
                if (box.empty()) { send_all(fd, tag + " NO geçersiz mailbox\r\n"); continue; }
                size_t total = 0, recent = 0;
                count_maildir(box, total, recent);
                if (mbname.size() >= 2 && mbname.front()=='"' && mbname.back()=='"')
                    mbname = mbname.substr(1, mbname.size()-2);
                send_all(fd, "* STATUS \"" + mbname + "\" (MESSAGES " + std::to_string(total) +
                             " RECENT " + std::to_string(recent) + ")\r\n" +
                             tag + " OK STATUS tamamlandı\r\n");
            }
            // ── M3: FETCH — mesaj içeriği oku (headers/body/flags/size/uid) ──
            else if (cmd == "FETCH") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO önce SELECT gerekli\r\n"); continue; }
                // args: "<seq-set> <items>"  (items parantezli veya tek)
                size_t sp = args.find(' ');
                if (sp == std::string::npos) { send_all(fd, tag + " BAD FETCH argümanı\r\n"); continue; }
                std::string seqset = args.substr(0, sp);
                std::string items  = args.substr(sp + 1);
                for (char& ch : items) ch = (char)std::toupper((unsigned char)ch);

                auto& msgs = messages;   // kararlı snapshot
                size_t lo = 0, hi = 0;
                if (!parse_seqset(seqset, msgs.size(), lo, hi)) {
                    send_all(fd, tag + " OK FETCH tamamlandı (boş)\r\n"); continue;
                }
                bool want_flags = items.find("FLAGS") != std::string::npos;
                bool want_uid   = items.find("UID")   != std::string::npos;
                bool want_size  = items.find("RFC822.SIZE") != std::string::npos;
                // "RFC822" tam gövde ister; "RFC822.SIZE"/"RFC822.HEADER" içermez.
                bool rfc822_full = (items.find("RFC822") != std::string::npos &&
                                    items.find("RFC822.") == std::string::npos);
                bool want_hdr   = items.find("BODY[HEADER]")!= std::string::npos || items.find("RFC822.HEADER") != std::string::npos;
                bool want_text  = items.find("BODY[TEXT]")  != std::string::npos;
                bool want_full  = items.find("BODY[]") != std::string::npos || rfc822_full;

                // Her mesajı TEK TEK işle + gönder — bellek bir mesaja sınırlı (OOM koruması)
                for (size_t i = lo; i <= hi; i++) {
                    std::string raw;
                    if (!read_file_capped(msgs[i-1], raw)) continue;  // dev/okunamayan mesaj → atla
                    std::string hdr, body; split_header_body(raw, hdr, body);

                    std::string parts;
                    // TÜM flag'leri raporla (STORE ile tutarlı) — sadece \Seen değil
                    if (want_flags) parts += "FLAGS " + maildir_to_imap_flags(maildir_flags(msgs[i-1])) + " ";
                    if (want_uid)   parts += "UID " + std::to_string(i) + " ";
                    if (want_size)  parts += "RFC822.SIZE " + std::to_string(raw.size()) + " ";
                    std::string literal;
                    if (want_full)      { parts += "BODY[] ";       literal = raw;  }
                    else if (want_hdr)  { parts += "BODY[HEADER] "; literal = hdr;  }
                    else if (want_text) { parts += "BODY[TEXT] ";   literal = body; }

                    std::string resp = "* " + std::to_string(i) + " FETCH (" ;
                    // parts sonundaki boşluğu kırp
                    while (!parts.empty() && parts.back() == ' ') parts.pop_back();
                    resp += parts;
                    if (!literal.empty() || want_full || want_hdr || want_text)
                        resp += " {" + std::to_string(literal.size()) + "}\r\n" + literal;
                    resp += ")\r\n";
                    if (!send_all(fd, resp)) break;
                }
                send_all(fd, tag + " OK FETCH tamamlandı\r\n");
            }
            // ── M4a: STORE — flag değiştir (okundu işaretle vb.) ─────────────
            else if (cmd == "STORE") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO önce SELECT gerekli\r\n"); continue; }
                // args: "<seq-set> <op>FLAGS[.SILENT] (<flags>)"
                size_t sp = args.find(' ');
                if (sp == std::string::npos) { send_all(fd, tag + " BAD STORE argümanı\r\n"); continue; }
                std::string seqset = args.substr(0, sp);
                std::string rest   = args.substr(sp + 1);
                std::string rest_u = rest; for (char& c : rest_u) c = (char)std::toupper((unsigned char)c);
                char op = '=';
                if (!rest_u.empty() && rest_u[0] == '+') op = '+';
                else if (!rest_u.empty() && rest_u[0] == '-') op = '-';
                bool silent = rest_u.find(".SILENT") != std::string::npos;
                // parantez içi flag listesi
                size_t lp = rest.find('('), rp = rest.rfind(')');
                std::string flaglist = (lp != std::string::npos && rp != std::string::npos && rp > lp)
                                       ? rest.substr(lp + 1, rp - lp - 1) : "";
                // istenen Maildir harflerini topla
                std::string want; { std::istringstream fs2(flaglist); std::string f;
                    while (fs2 >> f) { char m = imap_to_maildir_flag(f); if (m) want += m; } }

                auto& msgs = messages;   // kararlı snapshot
                size_t lo = 0, hi = 0;
                if (!parse_seqset(seqset, msgs.size(), lo, hi)) { send_all(fd, tag + " OK STORE tamamlandı (boş)\r\n"); continue; }

                std::string out;
                for (size_t i = lo; i <= hi; i++) {
                    std::string cur = maildir_flags(msgs[i-1]);
                    std::string nf;
                    if (op == '=') nf = want;
                    else if (op == '+') { nf = cur; for (char c : want) if (nf.find(c)==std::string::npos) nf += c; }
                    else { nf = cur; for (char c : want) { size_t k = nf.find(c); if (k!=std::string::npos) nf.erase(k,1); } }
                    std::string np = apply_flags(msgs[i-1], nf);
                    if (np.empty()) continue;  // eşzamanlı değişim → atla
                    msgs[i-1] = np;            // aynı indekste yolu güncelle (SEQ SABİT KALIR)
                    if (!silent)
                        out += "* " + std::to_string(i) + " FETCH (FLAGS " + maildir_to_imap_flags(normalize_flags(nf)) + ")\r\n";
                }
                out += tag + " OK STORE tamamlandı\r\n";
                send_all(fd, out);
            }
            // ── M4a: EXPUNGE — \Deleted işaretli mesajları kalıcı sil ─────────
            else if (cmd == "EXPUNGE") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO önce SELECT gerekli\r\n"); continue; }
                // RFC 3501: EXPUNGE yanıtları AZALAN sequence sırasında; snapshot'tan
                // da azalan sırada sil ki indeksler tutarlı kaysın (sequence yalnız
                // BURADA değişir — kararlılık kuralı).
                std::string out;
                std::error_code ec;
                for (size_t i = messages.size(); i >= 1; i--) {
                    if (maildir_flags(messages[i-1]).find('T') != std::string::npos) {
                        fs::remove(messages[i-1], ec);
                        if (!ec) {
                            messages.erase(messages.begin() + (i - 1));  // snapshot'tan çıkar
                            out += "* " + std::to_string(i) + " EXPUNGE\r\n";
                        }
                    }
                }
                out += tag + " OK EXPUNGE tamamlandı\r\n";
                send_all(fd, out);
            }
            // ── M4b: APPEND — mesaj ekle (Sent'e kaydet, taslak yükle) ───────
            // GÜVENLİK: literal {N} client-kontrollü → MAX_LITERAL cap (OOM),
            // guarded parse, path-traversal (resolve_mailbox), atomik yazma.
            else if (cmd == "APPEND") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                // args: "<mailbox> [(<flags>)] [\"date\"] {<size>}"
                // Literal boyutu: son '{' ... '}' (opsiyonel '+' non-sync)
                size_t lb = args.rfind('{'), rb = args.rfind('}');
                if (lb == std::string::npos || rb == std::string::npos || rb < lb) {
                    send_all(fd, tag + " BAD APPEND literal {N} gerekli\r\n"); continue;
                }
                std::string szs = args.substr(lb + 1, rb - lb - 1);
                bool nonsync = !szs.empty() && szs.back() == '+';
                if (nonsync) szs.pop_back();
                size_t litsize = 0;
                if (szs.empty() || szs.find_first_not_of("0123456789") != std::string::npos) {
                    send_all(fd, tag + " BAD APPEND geçersiz literal boyutu\r\n"); continue;
                }
                try { litsize = std::stoull(szs); }
                catch (...) { send_all(fd, tag + " BAD APPEND literal boyutu\r\n"); continue; }
                // OOM koruması — cap aşımı: veriyi HİÇ okumadan reddet
                if (litsize > imap_max_literal()) {
                    send_all(fd, tag + " NO [TOOBIG] APPEND mesaj çok büyük\r\n"); continue;
                }
                // mailbox adı (ilk token)
                std::string head = args.substr(0, lb);
                std::string mbname; { std::istringstream hs(head); hs >> mbname; }
                std::string box = resolve_mailbox(maildir_root, mbname);
                if (box.empty()) { send_all(fd, tag + " NO [TRYCREATE] geçersiz mailbox\r\n"); continue; }
                // flags (parantez içi, opsiyonel)
                std::string mflags;
                size_t fp = head.find('(');
                if (fp != std::string::npos) {
                    size_t fpe = head.find(')', fp);
                    if (fpe != std::string::npos) {
                        std::istringstream fs2(head.substr(fp + 1, fpe - fp - 1)); std::string f;
                        while (fs2 >> f) { char m = imap_to_maildir_flag(f); if (m) mflags += m; }
                    }
                }
                // Senkron literal: devam isteği gönder
                if (!nonsync) { if (!send_all(fd, "+ literal verisi bekleniyor\r\n")) break; }
                // Tam litsize byte oku (bounded — cap zaten doğrulandı)
                std::string data;
                if (!read_exact(fd, litsize, data)) break;  // bağlantı/timeout
                // literal sonrası satır kalıntısını (CRLF) yut
                std::string tail; read_line(fd, tail);
                // Atomik yaz
                if (maildir_append(box, mflags, data))
                    send_all(fd, tag + " OK [APPENDUID 1 1] APPEND tamamlandı\r\n");
                else
                    send_all(fd, tag + " NO APPEND yazılamadı\r\n");
            }
            // ── M4c: SEARCH — ölçütlere uyan mesaj sıra numaralarını döndür ──
            else if (cmd == "SEARCH") {
                if (!authenticated) { send_all(fd, tag + " NO önce LOGIN gerekli\r\n"); continue; }
                if (selected.empty()) { send_all(fd, tag + " NO önce SELECT gerekli\r\n"); continue; }

                // Tırnakları dikkate alan basit tokenizer (SEARCH FROM "a b" ...)
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

                // Ölçütleri ayrıştır (hepsi AND). Kötü sözdizimi → BAD.
                struct Crit { std::string kw, a1, a2; size_t lo = 0, hi = 0; };
                std::vector<Crit> crits;
                bool bad = false, needs_content = false;
                for (size_t k = 0; k < tok.size(); k++) {
                    std::string K = up(tok[k]);
                    if (K == "CHARSET") { k++; continue; }           // CHARSET <name> — atla
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
                        if (!parse_seqset(set, messages.size(), c.lo, c.hi)) { c.lo = 1; c.hi = 0; } // eşleşmez
                        crits.push_back(c);
                    }
                    else { bad = true; break; }
                }
                if (bad) { send_all(fd, tag + " BAD SEARCH ölçütü çözümlenemedi\r\n"); continue; }

                auto hdr_field = [&](const std::string& hdr, const std::string& field) {
                    // "field:" ile başlayan satırların değerini (küçük harf) birleştir
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
                send_all(fd, result + "\r\n" + tag + " OK SEARCH tamamlandı\r\n");
            }
            else {
                send_all(fd, tag + " BAD bilinmeyen komut\r\n");
                if (++errors >= imap_max_errors()) break;
            }
        }
    }

    void accept_loop(int lfd, bool implicit_tls) {
        while (running.load()) {
            int cfd = (int)::accept(lfd, nullptr, nullptr);
            if (cfd < 0) { if (!running.load()) break; continue; }
            // Eşzamanlı bağlantı sınırı (DoS)
            if (conn_count.load() >= imap_max_conn()) {
                // Not: implicit-TLS portunda handshake öncesi düz-metin BYE
                //  gönderemeyiz; sadece kapat.
                if (!implicit_tls) send_all(cfd, "* BYE çok fazla bağlantı, sonra deneyin\r\n");
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

void ImapServer::start() {
    impl_->listen_fd = Impl::make_server_fd(impl_->port_imap);
    if (impl_->listen_fd < 0) {
        Logger::instance().log(LogLevel::LOG_ERROR, "IMAP",
            "port " + std::to_string(impl_->port_imap) + " dinlenemedi");
        return;
    }
    impl_->ssl_ctx = Impl::make_ssl_ctx();   // null = sertifika env yok (düz-metin dev)
    impl_->running = true;
    impl_->accept_thread = std::thread([this]() { impl_->accept_loop(impl_->listen_fd, false); });

    // IMAPS (implicit TLS) — yalnız sertifika + port varsa.
    if (impl_->ssl_ctx && impl_->port_imaps > 0) {
        impl_->listen_fd_tls = Impl::make_server_fd(impl_->port_imaps);
        if (impl_->listen_fd_tls >= 0) {
            impl_->accept_thread_tls = std::thread([this]() { impl_->accept_loop(impl_->listen_fd_tls, true); });
            Logger::instance().log(LogLevel::LOG_INFO, "IMAP",
                "IMAPS (implicit TLS) dinliyor — port " + std::to_string(impl_->port_imaps));
        }
    }
    Logger::instance().log(LogLevel::LOG_INFO, "IMAP",
        std::string("IMAP4rev1 dinliyor — port ") + std::to_string(impl_->port_imap) +
        (impl_->ssl_ctx ? " (STARTTLS aktif)" : " (TLS yapılandırılmamış — düz-metin)"));
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

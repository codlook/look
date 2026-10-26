#pragma once
// Dosya sürücüsünün oturum dosyaları (LOOK_SESSION_DRIVER=file, varsayılan).
//
// GÜVENLİK (analizci 1.0.36'da, biz 1.0.37'de doğruladık): dosyalar herkesin listeleyebildiği
// /tmp'ye "look_sess_<kimlik>" adıyla 0644 izinle yazılıyordu. Aynı sunucudaki her kullanıcı
// kimlikleri ve içerikleri okuyup oturumu çalabiliyordu; /tmp'ye kendi yazdığı bir
// look_sess_<kimlik> dosyası (içinde user=admin) çerezle gönderilince oturum olarak kabul
// ediliyordu. Şimdi (tmux / ssh-agent kuralı):
//   * dizin yalnız bu sürecin kullanıcısına ait ve 0700 olmalı, sembolik bağlantı olmamalı —
//     değilse kullanılmaz, neden söylenir. Varsayılan dizin: session_dir_path() (başkasının önceden
//     yaratamayacağı ilk yer; yoksa 0700 ile yaratılır); LOOK_SESSION_DIR aynı denetimlerle.
//   * süresi dolan (LOOK_SESSION_TTL) dosya okunmaz; arka planda düzenli silinir (session_files_gc).
//   * dosyalar 0600, sembolik bağlantı izlenmeden (O_NOFOLLOW), geçici dosya + rename ile yazılır;
//   * okunan dosya bu kullanıcıya ait, 0600'den gevşek olmayan, tek bağlantılı düz dosya olmalı —
//     değilse oturum yok sayılır (başkasının bıraktığı dosya asla oturum olmaz).
// Windows: %TEMP% zaten kullanıcıya özel; izin denetimi yok, dosyalar %TEMP%\look-sessions altında.
#include <string>
#include <stdexcept>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <atomic>
#include <ctime>
#include <thread>
#ifndef _WIN32
#  include <cerrno>
#  include <cstring>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#else
#  include <direct.h>
#endif

namespace look {

// Dizin uygun mu; değilse why'a İngilizce neden. create: yoksa 0700 ile yarat.
inline bool session_dir_ok(const std::string& dir, bool create, std::string& why) {
#ifndef _WIN32
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (errno != ENOENT || !create) {
            why = "session directory " + dir + " is not accessible (" + std::strerror(errno) + ")";
            return false;
        }
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            why = "could not create session directory " + dir + " (" + std::strerror(errno) + ")";
            return false;
        }
        if (::lstat(dir.c_str(), &st) != 0) {
            why = "session directory " + dir + " is not accessible (" + std::strerror(errno) + ")";
            return false;
        }
    }
    const char* fix = ". Remove it, or set LOOK_SESSION_DIR to a directory owned by this user with mode 0700.";
    if (S_ISLNK(st.st_mode)) { why = "session directory " + dir + " is a symbolic link; refusing to use it" + fix; return false; }
    if (!S_ISDIR(st.st_mode)) { why = "session directory " + dir + " is not a directory" + fix; return false; }
    if (st.st_uid != ::geteuid()) {
        why = "session directory " + dir + " is owned by uid " + std::to_string(st.st_uid) +
              ", not by this process (uid " + std::to_string(::geteuid()) + "); refusing to use it" + fix;
        return false;
    }
    if ((st.st_mode & 077) != 0) {
        char m[8]; std::snprintf(m, sizeof(m), "%04o", (unsigned)(st.st_mode & 07777));
        why = "session directory " + dir + " has mode " + m + "; it must be 0700 (only this user can read the sessions)" + fix;
        return false;
    }
    return true;
#else
    if (create) _mkdir(dir.c_str());
    (void)why;
    return true;
#endif
}

#ifndef _WIN32
// Var olan, bu kullanıcıya ait bir dizin mi (bağlantı izlenir: $HOME bir bağlantı olabilir).
inline bool session_dir_owned(const char* p) {
    struct stat st{};
    return p && *p && ::stat(p, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid();
}
#endif

// Kullanılacak dizin: LOOK_SESSION_DIR; yoksa BAŞKA BİR KULLANICININ ÖNCEDEN YARATAMAYACAĞI ilk yer:
//   $STATE_DIRECTORY/sessions (systemd StateDirectory=; kalıcı),
//   $XDG_RUNTIME_DIR/look-sessions (/run/user/<uid>, yalnız o kullanıcı),
//   $HOME/.look/sessions ($HOME bu kullanıcıya aitse — Debian'da www-data'nın /var/www'su root'un),
//   en son ${TMPDIR:-/tmp}/look-sessions-<uid>.
// 1.0.38 doğrudan /tmp'yi kullanıyordu: başka bir kullanıcı o dizini önceden yaratınca oturum
// kullanan her istek reddediliyordu (site ayakta, girişler çalışmıyor).
inline std::string session_dir_path() {
    if (const char* e = std::getenv("LOOK_SESSION_DIR")) if (*e) return e;
#ifndef _WIN32
    if (const char* s = std::getenv("STATE_DIRECTORY")) if (*s) {
        std::string first(s);
        first = first.substr(0, first.find(':'));   // birden çok StateDirectory: ilki
        if (session_dir_owned(first.c_str())) return first + "/sessions";
    }
    if (const char* x = std::getenv("XDG_RUNTIME_DIR")) if (session_dir_owned(x)) return std::string(x) + "/look-sessions";
    if (const char* h = std::getenv("HOME")) if (session_dir_owned(h)) return std::string(h) + "/.look/sessions";
    const char* t = std::getenv("TMPDIR");
    std::string base = (t && *t) ? t : "/tmp";
    return base + "/look-sessions-" + std::to_string(::geteuid());
#else
    const char* t = std::getenv("TEMP");
    return std::string(t ? t : ".") + "\\look-sessions";
#endif
}

// Eksik üst dizinler (ör. LOOK_SESSION_DIR=$HOME/.look/sessions-site) bu kullanıcı olarak 0700 ile
// yaratılır — kök (root) kullanıcının başkasının dizininde iş yapması gerekmez. Var olana dokunulmaz.
inline void session_dir_make_parents(const std::string& dir) {
#ifndef _WIN32
    for (size_t p = dir.find('/', 1); p != std::string::npos; p = dir.find('/', p + 1))
        ::mkdir(dir.substr(0, p).c_str(), 0700);
#else
    (void)dir;
#endif
}

// Denetlenmiş dizin (başarılı sonuç süreç boyunca saklanır); uygun değilse nedenle hata.
inline std::string session_dir() {
    static std::mutex mu;
    static std::string ok_dir;
    std::lock_guard<std::mutex> g(mu);
    if (!ok_dir.empty()) return ok_dir;
    std::string dir = session_dir_path(), why;
    session_dir_make_parents(dir);
    if (!session_dir_ok(dir, true, why)) throw std::runtime_error("session: " + why);
    ok_dir = dir;
    return ok_dir;
}

inline std::string session_file(const std::string& sid) {
#ifndef _WIN32
    return session_dir() + "/look_sess_" + sid;
#else
    return session_dir() + "\\look_sess_" + sid;
#endif
}

// Oku. Dosya yoksa — ya da bu sürecin yazdığı bir oturum dosyası değilse, ya da ttl_sec
// saniyedir yazılmamışsa (süresi dolmuş: silinir) — boş döner. Eskiden dosya sürücüsü
// LOOK_SESSION_TTL'yi hiç uygulamıyordu: eski bir dosya hâlâ geçerli oturumdu (Redis EXPIRE ile
// siliyordu). Süre son YAZMADAN sayılır — Redis'teki SET ... EX ile aynı.
// foreign: dosya vardı ama reddedildi (çağıran uyarı yazabilir).
inline std::string session_file_read(const std::string& sid, bool* foreign = nullptr, long ttl_sec = 0) {
    if (foreign) *foreign = false;
    const std::string path = session_file(sid);
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { if (errno == ELOOP && foreign) *foreign = true; return ""; }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 077) != 0 || st.st_nlink != 1) {
        ::close(fd);
        if (foreign) *foreign = true;
        return "";
    }
    if (ttl_sec > 0 && (long long)std::time(nullptr) - (long long)st.st_mtime > ttl_sec) {
        ::close(fd);
        ::unlink(path.c_str());   // süresi dolmuş
        return "";
    }
    std::string out;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    ::close(fd);
    return out;
#else
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
#endif
}

// Yaz: aynı dizinde 0600 geçici dosya → içerik → rename (yarım okunan oturum olmaz; hedef bir
// sembolik bağlantıysa rename onu değiştirir, izlemez).
inline void session_file_write(const std::string& sid, const std::string& blob) {
    const std::string path = session_file(sid);
#ifndef _WIN32
    std::string tmpl = path + ".XXXXXX";
    int fd = ::mkstemp(&tmpl[0]);   // 0600, O_EXCL
    if (fd < 0) throw std::runtime_error(std::string("session: could not write the session file (") + std::strerror(errno) + ")");
    ::fchmod(fd, 0600);
    const char* p = blob.data(); size_t left = blob.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = errno; ::close(fd); ::unlink(tmpl.c_str());
            throw std::runtime_error(std::string("session: could not write the session file (") + std::strerror(e) + ")");
        }
        p += n; left -= (size_t)n;
    }
    if (::close(fd) != 0 || ::rename(tmpl.c_str(), path.c_str()) != 0) {
        int e = errno; ::unlink(tmpl.c_str());
        throw std::runtime_error(std::string("session: could not write the session file (") + std::strerror(e) + ")");
    }
#else
    std::ofstream f(path, std::ios::binary | std::ios::trunc); f << blob;
#endif
}

inline void session_file_remove(const std::string& sid) {
    std::remove(session_file(sid).c_str());
}

// Süresi dolan oturum dosyalarını sil. Eskiden hiç silinmiyordu: yeni ziyaretçinin oturumuna
// yazan her istek (her sayfada CSRF anahtarı) kalıcı bir dosya bırakıyordu — çerezsiz istekler,
// giriş yapmadan, diski ve inode'ları doldurabiliyordu (ölçüldü: 3000 istek, 3000 dosya).
// Süreç başına en çok 5 dakikada bir (TTL daha kısaysa TTL'de bir), arka planda bir iş
// parçacığında: istek taramayı beklemez. Yarım kalmış geçici dosyalar (look_sess_<id>.XXXXXX)
// bir saat sonra silinir. Yalnız bu kullanıcının düz dosyalarına dokunulur.
inline void session_files_gc(long ttl_sec) {
#ifndef _WIN32
    if (ttl_sec <= 0) return;
    static std::atomic<long long> last{0};
    static std::atomic<bool> running{false};
    const long long now = (long long)std::time(nullptr);
    const long long every = ttl_sec < 300 ? ttl_sec : 300;
    long long prev = last.load();
    if (now - prev < every || !last.compare_exchange_strong(prev, now)) return;
    bool idle = false;
    if (!running.compare_exchange_strong(idle, true)) return;
    std::string dir;
    try { dir = session_dir(); } catch (...) { running = false; return; }
    auto sweep = [dir, ttl_sec, now]() {
        if (DIR* d = ::opendir(dir.c_str())) {
            const int dfd = ::dirfd(d);
            while (struct dirent* e = ::readdir(d)) {
                if (std::strncmp(e->d_name, "look_sess_", 10) != 0) continue;
                struct stat st{};
                if (::fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) continue;
                const bool tmp = std::strchr(e->d_name, '.') != nullptr;
                const long long age = now - (long long)st.st_mtime;
                if (age > (tmp ? (ttl_sec > 3600 ? ttl_sec : 3600) : ttl_sec)) ::unlinkat(dfd, e->d_name, 0);
            }
            ::closedir(d);
        }
        running = false;
    };
    try { std::thread(sweep).detach(); }
    catch (...) { sweep(); }   // iş parçacığı açılamadı (bellek sınırı): bu istekte tara
#else
    (void)ttl_sec;
#endif
}

// Başlangıç denetimi: LOOK_SESSION_DIR açıkça verilmişse (yöneticinin ayarı) uygun değilse
// süreç başlamaz. Boş dönüş = sorun yok. Varsayılan dizin ilk kullanımda denetlenir: oturum
// kullanmayan bir uygulama, /tmp'de başkasının önceden yarattığı bir dizin yüzünden durmasın.
inline std::string session_dir_startup_problem() {
    const char* d = std::getenv("LOOK_SESSION_DRIVER");
    if (d && std::string(d) == "redis") return "";
    const char* e = std::getenv("LOOK_SESSION_DIR");
    if (!e || !*e) return "";
    std::string why;
    session_dir_make_parents(e);
    return session_dir_ok(e, true, why) ? "" : why;
}

} // namespace look

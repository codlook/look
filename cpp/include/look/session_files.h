#pragma once
// Session files of the file driver (LOOK_SESSION_DRIVER=file, the default).
//
// SECURITY (fixed in 1.0.37): files were written to the world-listable
// /tmp as "look_sess_<id>" with mode 0644. Any user on the same server
// could read ids and contents and steal sessions; a look_sess_<id> file they wrote to /tmp
// themselves (containing user=admin) was accepted as a session when sent in the cookie.
// Now (tmux / ssh-agent rule):
//   * the directory must belong only to this process's user, be 0700, and not be a symlink —
//     otherwise it is not used and the reason is stated. Default directory: session_dir_path() (the first place someone else
//     cannot pre-create; created with 0700 if missing); LOOK_SESSION_DIR gets the same checks.
//   * an expired (LOOK_SESSION_TTL) file is not read; it is deleted periodically in the background (session_files_gc).
//   * files are written 0600, without following symlinks (O_NOFOLLOW), via temp file + rename;
//   * a file that is read must be a regular file owned by this user, no looser than 0600, with a single link —
//     otherwise the session is ignored (a file left by someone else never becomes a session).
// Windows: %TEMP% is already per-user; no permission checks, files live under %TEMP%\look-sessions.
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
#  include <sys/file.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#else
#  include <direct.h>
#endif

namespace look {

// Is the directory suitable; if not, an English reason in why. create: create with 0700 if missing.
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
// Is it an existing directory owned by this user (links are followed: $HOME may be a link).
inline bool session_dir_owned(const char* p) {
    struct stat st{};
    return p && *p && ::stat(p, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid();
}
#endif

// Directory to use: LOOK_SESSION_DIR; otherwise the first place ANOTHER USER CANNOT PRE-CREATE:
//   $STATE_DIRECTORY/sessions (systemd StateDirectory=; persistent),
//   $XDG_RUNTIME_DIR/look-sessions (/run/user/<uid>, only that user),
//   $HOME/.look/sessions (if $HOME belongs to this user — on Debian www-data's /var/www is root's),
//   finally ${TMPDIR:-/tmp}/look-sessions-<uid>.
// 1.0.38 used /tmp directly: when another user pre-created that directory, every request that
// used sessions was rejected (site up, logins broken).
inline std::string session_dir_path() {
    if (const char* e = std::getenv("LOOK_SESSION_DIR")) if (*e) return e;
#ifndef _WIN32
    if (const char* s = std::getenv("STATE_DIRECTORY")) if (*s) {
        std::string first(s);
        first = first.substr(0, first.find(':'));   // with multiple StateDirectory entries: the first
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

// Missing parent directories (e.g. LOOK_SESSION_DIR=$HOME/.look/sessions-site) are created as this user with 0700 —
// root need not work inside someone else's directory. Existing ones are left alone.
inline void session_dir_make_parents(const std::string& dir) {
#ifndef _WIN32
    for (size_t p = dir.find('/', 1); p != std::string::npos; p = dir.find('/', p + 1))
        ::mkdir(dir.substr(0, p).c_str(), 0700);
#else
    (void)dir;
#endif
}

// Checked directory (a successful result is kept for the lifetime of the process); error with reason if unsuitable.
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

// Read. Returns empty if the file does not exist — or is not a session file written by this process, or if it was
// not written within ttl_sec seconds (expired: deleted). Previously the file driver
// never enforced LOOK_SESSION_TTL: an old file was still a valid session (Redis expired it
// with EXPIRE). The time counts from the last WRITE — same as SET ... EX in Redis.
// foreign: the file existed but was rejected (the caller may log a warning).
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
        ::unlink(path.c_str());   // expired
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

// Did this process issue this session (file exists, ours, not expired)? Same checks as session_file_read;
// content is not read. An empty session (no data written) also exists — read returning ""
// cannot tell "absent" from "empty". Strict mode of session::start uses this (fixation protection).
inline bool session_file_exists(const std::string& sid, long ttl_sec = 0) {
    const std::string path = session_file(sid);
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() &&
              (st.st_mode & 077) == 0 && st.st_nlink == 1 &&
              !(ttl_sec > 0 && (long long)std::time(nullptr) - (long long)st.st_mtime > ttl_sec);
    ::close(fd);
    return ok;
#else
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
#endif
}

// session_file_exists + content in a single open: true = a non-expired session issued by this process
// (content may be empty). session::start uses this: at the same cost as the existence check it also gets
// the data at the start of the request (for a concurrent regenerate, that request's copy — see web_stdlib).
inline bool session_file_read_if_exists(const std::string& sid, std::string& out, long ttl_sec = 0) {
    out.clear();
    const std::string path = session_file(sid);
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() &&
              (st.st_mode & 077) == 0 && st.st_nlink == 1 &&
              !(ttl_sec > 0 && (long long)std::time(nullptr) - (long long)st.st_mtime > ttl_sec);
    if (ok) {
        char buf[65536];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
    }
    ::close(fd);
    return ok;
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
#endif
}

// Write: 0600 temp file in the same directory → content → rename (no half-read session; if the target is a
// symlink, rename replaces it rather than following it).
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

// Update — ONLY if the session still exists. false: no session (deleted by session::destroy or
// regenerate while this request was running). Previously session::set did check-exists → load → write: if another
// tab logged out in between, the write re-created the file with OLD data (the logout was undone).
// Now: the existing file is opened, locked (flock), and replaced with the new one only if still linked (nlink);
// deletion uses the same lock. If the file was replaced by another update while waiting for the lock,
// the new one is looked up again (last writer wins — as before).
inline bool session_file_update(const std::string& sid, const std::string& blob) {
#ifndef _WIN32
    const std::string path = session_file(sid);
    for (int attempt = 0; attempt < 16; ++attempt) {
        int lfd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (lfd < 0) return false;                         // deleted
        while (::flock(lfd, LOCK_EX) != 0 && errno == EINTR) {}
        struct stat st{};
        if (::fstat(lfd, &st) != 0 || st.st_nlink == 0) { ::close(lfd); continue; }   // changed/deleted: look again
        try { session_file_write(sid, blob); } catch (...) { ::close(lfd); throw; }
        ::close(lfd);                                       // the lock is released together with the old file
        return true;
    }
    throw std::runtime_error("session: the session file kept changing while it was being written");
#else
    if (!session_file_exists(sid)) return false;
    session_file_write(sid, blob);
    return true;
#endif
}

inline void session_file_remove(const std::string& sid) {
#ifndef _WIN32
    // Same lock as update: no deletion before an update holding the lock finishes; afterward
    // the file on the path (the one it wrote) is deleted — logout always wins.
    // If the locked file was replaced in the meantime (nlink 0), the new one on the path is locked.
    const std::string path = session_file(sid);
    for (int attempt = 0; attempt < 16; ++attempt) {
        int lfd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (lfd < 0) { ::unlink(path.c_str()); return; }   // missing (or a link: remove anyway)
        while (::flock(lfd, LOCK_EX) != 0 && errno == EINTR) {}
        struct stat st{};
        if (::fstat(lfd, &st) == 0 && st.st_nlink == 0) { ::close(lfd); continue; }
        ::unlink(path.c_str());
        ::close(lfd);
        return;
    }
    ::unlink(path.c_str());
#else
    std::remove(session_file(sid).c_str());
#endif
}

// Delete expired session files. Previously never deleted: every request writing to a new visitor's
// session (a CSRF key on every page) left a permanent file — cookieless requests, without
// logging in, could fill disk and inodes (measured: 3000 requests, 3000 files).
// When: at most every 5 minutes (every TTL if shorter), once for ALL PROCESSES. The time of the last
// cleanup is kept in the .gc-stamp file in the directory and locked (flock): in 1.0.39 it was
// in process memory — in CGI every request is a new process, so cleanup either started on every request
// or died half-done when the process exited (measured: after expiry none of 320 files
// was deleted). In a long-lived server in a background thread; in a short-lived process
// (CGI: session_gc_short_lived) after the response is sent via session_gc_run_pending.
// Half-written temp files (look_sess_<id>.XXXXXX) are deleted after at least an hour. Only this
// user's regular files are touched.
inline std::atomic<bool>& session_gc_short_lived_flag() { static std::atomic<bool> v{false}; return v; }
inline std::atomic<long>& session_gc_pending_ttl()    { static std::atomic<long> v{0};     return v; }
inline void session_gc_short_lived(bool on) { session_gc_short_lived_flag() = on; }
#ifndef _WIN32

// Can this process take over cleanup: yes if the stamp is missing or old and the lock can be acquired.
// Returns: the locked stamp fd (closed when the scan finishes) or -1.
inline int session_gc_claim(const std::string& dir, long long every, long long now) {
    const std::string stamp = dir + "/.gc-stamp";
    int fd = ::open(stamp.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st{};
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 || ::fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_size > 0 && now - (long long)st.st_mtime < every)) {   // size 0 = never scanned
        ::close(fd);
        return -1;
    }
    if (::pwrite(fd, "1", 1, 0) != 1 || ::futimens(fd, nullptr) != 0) { ::close(fd); return -1; }
    return fd;
}

inline void session_gc_sweep(const std::string& dir, long ttl_sec, long long now) {
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
}

inline void session_gc_run(long ttl_sec) {
    const long long now = (long long)std::time(nullptr);
    const long long every = ttl_sec < 300 ? ttl_sec : 300;
    std::string dir;
    try { dir = session_dir(); } catch (...) { return; }
    int fd = session_gc_claim(dir, every, now);
    if (fd < 0) return;   // interval not elapsed or another process is scanning
    session_gc_sweep(dir, ttl_sec, now);
    ::close(fd);
}
#endif

inline void session_files_gc(long ttl_sec) {
#ifndef _WIN32
    if (ttl_sec <= 0) return;
    static std::atomic<long long> last{0};
    static std::atomic<bool> running{false};
    const long long now = (long long)std::time(nullptr);
    const long long every = ttl_sec < 300 ? ttl_sec : 300;
    long long prev = last.load();
    if (now - prev < every || !last.compare_exchange_strong(prev, now)) return;   // in-process shortcut
    if (session_gc_short_lived_flag()) { session_gc_pending_ttl() = ttl_sec; return; }   // after the response
    bool idle = false;
    if (!running.compare_exchange_strong(idle, true)) return;
    auto job = [ttl_sec]() { session_gc_run(ttl_sec); running = false; };
    try { std::thread(job).detach(); }
    catch (...) { job(); }   // could not start a thread (memory limit): scan on this request
#else
    (void)ttl_sec;
#endif
}

// Short-lived process (CGI): after the response is sent, scan if cleanup is due on this request.
inline void session_gc_run_pending() {
#ifndef _WIN32
    long ttl = session_gc_pending_ttl().exchange(0);
    if (ttl > 0) session_gc_run(ttl);
#endif
}

// Startup check: if LOOK_SESSION_DIR was given explicitly (administrator's setting) and is unsuitable,
// the process does not start. Empty return = no problem. The default directory is checked on first use: an app
// that does not use sessions should not stop because of a directory someone else pre-created in /tmp.
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

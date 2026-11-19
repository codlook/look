#include "look/mem_budget.h"   // mem_reserve — budget before a large allocation
#include "look/stdlib.h"
#include "look/web.h"
#include "look/request_scope.h"   // current_request_web: the request's DOCUMENT_ROOT value
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>

namespace fs = std::filesystem;

#ifndef _WIN32
#  include <sys/stat.h>
// Process umask — read once during static initialization (before main, single thread): umask()
// does read-and-write together, so calling it during a request would affect another thread's file.
static const mode_t g_process_umask = [] { mode_t m = ::umask(0); ::umask(m); return m; }();
#endif

namespace look {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string get_upload_root() {
    const char* ud = std::getenv("UPLOAD_DIR");
    if (ud && *ud) return ud;
    // Default: storage/uploads next to the working directory
    return (fs::current_path() / "storage" / "uploads").string();
}

static std::string get_upload_url() {
    const char* uu = std::getenv("UPLOAD_URL");
    return uu ? uu : "";
}

// Web root detection — the request's DOCUMENT_ROOT (FastCGI parameter / CGI environment), otherwise the process environment.
// In FastCGI the request parameters are no longer written to the process environment (fcgi_main.cpp): the value is read from the request's
// context; otherwise this check would silently be disabled in FastCGI.
static std::string get_web_root() {
    if (auto* w = look::current_request_web(); w && !w->document_root.empty()) return w->document_root;
    const char* dr = std::getenv("DOCUMENT_ROOT");
    return dr ? dr : "";
}

// Security: is the upload path under the web root?
static bool is_under_web_root(const std::string& path) {
    std::string wr = get_web_root();
    if (wr.empty()) return false;
    fs::path p  = fs::weakly_canonical(fs::path(path));
    fs::path wr_p = fs::weakly_canonical(fs::path(wr));
    auto [it, end] = std::mismatch(wr_p.begin(), wr_p.end(), p.begin());
    return it == wr_p.end();
}

// ── Path traversal guard — SAFE DEFAULT ──────────────────────────────────────
// file:: operations are restricted to a directory tree (prevents reading
// `../../etc/passwd` via user input). In a language that is "distributed like PHP but
// thinks like Go" the default must be safe:
//   • if LOOK_FILE_ROOT is NOT set → restrict to the working directory (cwd) (safe).
//   • LOOK_FILE_ROOT=<path>       → restrict to that directory (widening, opt-in).
//   • LOOK_FILE_ROOT=*           → unrestricted (explicit opt-out; all responsibility is yours).
// (Internal modules — session/cache/jobs — do not use file::, they are not affected by this restriction.)
static std::string get_file_root() {
    static std::string root = []() -> std::string {
        const char* r = std::getenv("LOOK_FILE_ROOT");
        if (r && std::string(r) == "*") return "";     // explicitly unrestricted
        std::string base;
        if (r && *r) base = r;                          // user root
        else {                                          // DEFAULT: cwd
            std::error_code ec;
            base = fs::current_path(ec).string();
            if (ec) return "";                          // cwd could not be obtained → unrestricted
        }
        std::error_code ec;
        auto p = fs::weakly_canonical(fs::path(base), ec);
        return ec ? base : p.string();
    }();
    return root;
}

// Resolve the path component by component; EVERY symlink encountered is followed — even if its target does not exist (Go 1.24
// os.Root model: a link pointing outside the root is never followed). OLD HOLE: weakly_canonical
// could not resolve a nonexistent target, so it took a dangling link ("dangle" → /outside/new.txt) to be inside the root
// → file::put created a file OUTSIDE the root (measured). ".." is applied after the link is followed
// (same as POSIX resolution). 40-link limit (Linux ELOOP).
static fs::path resolve_all_links(const fs::path& abs, int depth = 0) {
    if (depth > 40) throw std::runtime_error("file: too many levels of symbolic links: " + abs.string());
    fs::path cur = abs.root_path();
    const fs::path rel = abs.relative_path();
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        const fs::path& comp = *it;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") { cur = cur.parent_path(); continue; }
        fs::path next = cur / comp;
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(next, ec)) && !ec) {
            fs::path tgt = fs::read_symlink(next, ec);
            if (ec) throw std::runtime_error("file: cannot read symbolic link: " + next.string());
            if (tgt.is_relative()) tgt = cur / tgt;
            for (auto j = std::next(it); j != rel.end(); ++j) tgt /= *j;
            return resolve_all_links(tgt, depth + 1);
        }
        cur = next;
    }
    return cur.lexically_normal();
}

static void assert_in_file_root(const std::string& path) {
    std::string root = get_file_root();
    if (root.empty()) return; // unrestricted
    std::error_code ec;
    // The path is FIRST made absolute relative to the cwd (relative "out.txt" is relative to the cwd — same resolution as the OS),
    // then every component and every symlink (including dangling) is resolved.
    fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) throw std::runtime_error("file: invalid path: " + path);
    fs::path resolved = resolve_all_links(abs);
    // Bring it to the SAME form as the root (the root was computed with weakly_canonical): on Windows, a drive letter / case
    // difference ("c:" ↔ "C:") would reject legitimate access. There is no link left in resolved, so it is safe.
    resolved = fs::weakly_canonical(resolved, ec);
    if (ec) throw std::runtime_error("file: invalid path: " + path);
    // Store root as a named variable — iterators from temporaries dangle.
    // Use 4-arg mismatch to guard against resolved being shorter than root.
    fs::path root_path(root);
    auto [it, it2] = std::mismatch(
        root_path.begin(), root_path.end(), resolved.begin(), resolved.end());
    if (it != root_path.end())
        throw std::runtime_error("file: access denied (path outside LOOK_FILE_ROOT): " + path);
}

// ── file:: Module ─────────────────────────────────────────────────────────────

Module make_file_module() {
    Module m;
    m.name = "file";

    // file::read(path) → string
    m.functions["read"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("file::read() requires path");
        std::string path = args[0].to_string();
        assert_in_file_root(path);
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("file::read(): cannot open: " + path);
        // Memory budget: the size is asked BEFORE the file is read (look/mem_budget.h) — so that a large file
        // does not exhaust memory before reaching a safe point. Reading makes several copies (ostringstream + value).
        {
            std::error_code ec;
            auto sz = std::filesystem::file_size(path, ec);
            if (!ec) look::mem_reserve((int64_t)sz * 2, "file::read()");
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return Value(ss.str());
    };

    // file::put(path, content) → bool
    m.functions["put"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("file::put() requires path and content");
        std::string path    = args[0].to_string();
        assert_in_file_root(path);
        std::string content = args[1].to_string();
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("file::put(): cannot open: " + path);
        f.write(content.data(), (std::streamsize)content.size());
        return Value(true);
    };

    // file::append(path, content) → bool
    m.functions["append"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("file::append() requires path and content");
        std::string path    = args[0].to_string();
        assert_in_file_root(path);
        std::string content = args[1].to_string();
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) throw std::runtime_error("file::append(): cannot open: " + path);
        f.write(content.data(), (std::streamsize)content.size());
        return Value(true);
    };

    // file::exists(path) → bool
    m.functions["exists"] = [](auto args) -> Value {
        if (args.empty()) return Value(false);
        std::string path = args[0].to_string();
        assert_in_file_root(path);
        return Value(fs::exists(path));
    };

    // file::list(path) → [ {name, dir, size}, … ]  — one directory level, sorted by name.
    // Flat (no recursion — that's the caller's loop) and fail-loud: a missing or non-directory
    // path throws rather than returning an empty list (a typo shouldn't look like an empty dir).
    // Same sandbox as the rest of file:: — listing a directory outside LOOK_FILE_ROOT is denied
    // (it would leak the directory structure), so the guard matters even more here.
    m.functions["list"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("file::list() requires path");
        std::string path = args[0].to_string();
        assert_in_file_root(path);
        std::error_code ec;
        if (!fs::is_directory(path, ec) || ec)
            throw std::runtime_error("file::list(): not a directory: " + path);
        std::vector<std::tuple<std::string, bool, int64_t>> entries;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            if (ec) break;
            std::error_code ec2;
            bool is_dir = e.is_directory(ec2);
            int64_t size = 0;
            if (!is_dir) { auto s = e.file_size(ec2); if (!ec2) size = (int64_t)s; }
            entries.emplace_back(e.path().filename().string(), is_dir, size);
        }
        if (ec) throw std::runtime_error("file::list(): cannot read directory: " + path);
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
        auto out = std::make_shared<ValueArray>();
        for (const auto& [name, is_dir, size] : entries) {
            auto rec = std::make_shared<ValueArray>();
            rec->push_back(Value::assoc_marker());
            rec->push_back(Value(std::string("name"))); rec->push_back(Value(name));
            rec->push_back(Value(std::string("dir")));  rec->push_back(Value(is_dir));
            rec->push_back(Value(std::string("size"))); rec->push_back(Value(size));
            out->push_back(Value(rec));
        }
        return Value(out);
    };

    // file::remove(path) → bool
    m.functions["remove"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("file::remove() requires path");
        std::string path = args[0].to_string();
        assert_in_file_root(path);
        return Value(fs::remove(path));
    };

    // file::size(path) → int (bytes)
    m.functions["size"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("file::size() requires path");
        std::string path = args[0].to_string();
        assert_in_file_root(path);
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec) throw std::runtime_error("file::size(): " + ec.message());
        return Value((int64_t)sz);
    };

    // file::store(file_array, subdir) → assoc array {path, url, sha256}
    // file_array: the return value of request::file()
    // subdir: a subfolder name such as "avatars", "documents" — cannot be a path
    m.functions["store"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("file::store() requires file and subdir");

        // validate file_array
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("file::store(): first argument must be a file from request::file()");

        auto& arr = *args[0].as_array();
        bool is_assoc = !arr.empty() && arr[0].is_assoc_marker();
        if (!is_assoc)
            throw std::runtime_error("file::store(): invalid file argument");

        // Extract the file information
        std::string temp_path, mime, sha256;
        for (size_t i = 1; i + 1 < arr.size(); i += 2) {
            std::string key = arr[i].to_string();
            if (key == "path")   temp_path = arr[i+1].to_string();
            if (key == "mime")   mime      = arr[i+1].to_string();
            if (key == "sha256") sha256    = arr[i+1].to_string();
        }
        if (temp_path.empty()) throw std::runtime_error("file::store(): missing temp path");

        // Subfolder name — cannot contain path characters
        std::string subdir = args[1].to_string();
        if (subdir.find('/') != std::string::npos ||
            subdir.find('\\') != std::string::npos ||
            subdir.find("..") != std::string::npos)
            throw std::runtime_error("file::store(): subdir must be a simple name, not a path");

        // Create the target folder
        std::string upload_root = get_upload_root();
        fs::path dest_dir = fs::path(upload_root) / subdir;

        // Security check for writing under the web root
        // Skip if UPLOAD_DIR is explicitly set (allow_mime already did MIME validation)
        bool upload_dir_explicit = (std::getenv("UPLOAD_DIR") && *std::getenv("UPLOAD_DIR"));
        if (!upload_dir_explicit && is_under_web_root(dest_dir.string()))
            throw std::runtime_error("file::store(): upload dir cannot be under web root — set UPLOAD_DIR to a path outside web root");

        // Only the temporary file this request received via request::file() is stored; the path, sha256 and mime
        // are taken from LOOK's own record, NOT from the caller's array. OLD HOLE: if the array was built by hand,
        // "path" could move any file on the server (.env) into the upload folder and have it served from UPLOAD_URL;
        // since the "sha256" field enters the file name, "../../" could write outside the folder.
        {
            const UploadedFile* own = nullptr;
            if (auto* w = look::current_request_web())
                for (const auto& [name, uf] : w->uploaded_files)
                    if (uf.valid && uf.temp_path == temp_path) { own = &uf; break; }
            if (!own)
                throw std::runtime_error("file::store(): the first argument must be a file from request::file() in this request");
            temp_path = own->temp_path; mime = own->mime; sha256 = own->sha256;
        }

        fs::create_directories(dest_dir);

        // Determine the extension from the MIME (the temp file already has the right extension)
        fs::path tmp = fs::path(temp_path);
        std::string ext = tmp.extension().string();

        // use the first 32 characters of the sha256 as the file name
        std::string safe_name = sha256.substr(0, 32) + ext;
        fs::path dest = dest_dir / safe_name;

        // Move (if the same file already exists — deduplication)
        if (!fs::exists(dest)) {
            fs::rename(temp_path, dest);
#ifndef _WIN32
            // The temporary upload file is opened 0600 (so nobody else can read it in the shared /tmp). The stored
            // file is served by the web server via UPLOAD_URL: normal file permission (0666 & ~umask).
            ::chmod(dest.c_str(), 0666 & ~g_process_umask);
#endif
        } else
            fs::remove(temp_path);  // duplicate — delete temp

        // Public URL
        std::string base_url = get_upload_url();
        std::string url;
        if (!base_url.empty()) {
            if (base_url.back() != '/') base_url += '/';
            url = base_url + subdir + "/" + safe_name;
        }

        // Return value
        auto result = std::make_shared<ValueArray>();
        result->push_back(Value::assoc_marker());
        result->push_back(Value(std::string("path")));   result->push_back(Value(dest.string()));
        result->push_back(Value(std::string("name")));   result->push_back(Value(safe_name));
        result->push_back(Value(std::string("url")));    result->push_back(Value(url));
        result->push_back(Value(std::string("sha256"))); result->push_back(Value(sha256));
        result->push_back(Value(std::string("mime")));   result->push_back(Value(mime));
        return Value(result);
    };

    // file::upload_dir() → upload root string (informational)
    m.functions["upload_dir"] = [](auto) -> Value {
        return Value(get_upload_root());
    };

    return m;
}

} // namespace look

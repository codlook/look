#include "look/stdlib.h"
#include "look/web.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>

namespace fs = std::filesystem;

namespace look {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string get_upload_root() {
    const char* ud = std::getenv("UPLOAD_DIR");
    if (ud && *ud) return ud;
    // Varsayılan: çalışma dizininin yanında storage/uploads
    return (fs::current_path() / "storage" / "uploads").string();
}

static std::string get_upload_url() {
    const char* uu = std::getenv("UPLOAD_URL");
    return uu ? uu : "";
}

// Web root tespiti — DOCUMENT_ROOT env ile
static std::string get_web_root() {
    const char* dr = std::getenv("DOCUMENT_ROOT");
    return dr ? dr : "";
}

// Güvenlik: upload path web root altında mı?
static bool is_under_web_root(const std::string& path) {
    std::string wr = get_web_root();
    if (wr.empty()) return false;
    fs::path p  = fs::weakly_canonical(fs::path(path));
    fs::path wr_p = fs::weakly_canonical(fs::path(wr));
    auto [it, end] = std::mismatch(wr_p.begin(), wr_p.end(), p.begin());
    return it == wr_p.end();
}

// ── Path traversal guard ─────────────────────────────────────────────────────
// LOOK_FILE_ROOT: restricts file:: operations to a directory subtree.
// If unset: unrestricted (trusted server-side code — same as before).
// If set: any path escaping the root throws 403.
static std::string get_file_root() {
    static std::string root = []() -> std::string {
        const char* r = std::getenv("LOOK_FILE_ROOT");
        if (!r || !*r) return "";
        std::error_code ec;
        auto p = fs::weakly_canonical(fs::path(r), ec);
        return ec ? std::string(r) : p.string();
    }();
    return root;
}

static void assert_in_file_root(const std::string& path) {
    std::string root = get_file_root();
    if (root.empty()) return; // unrestricted
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(path), ec);
    if (ec) throw std::runtime_error("file: invalid path: " + path);
    auto [it, end] = std::mismatch(
        fs::path(root).begin(), fs::path(root).end(), resolved.begin());
    if (it != fs::path(root).end())
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
        return Value((int)sz);
    };

    // file::store(file_array, subdir) → assoc array {path, url, sha256}
    // file_array: request::file() dönüş değeri
    // subdir: "avatars", "documents" gibi alt klasör adı — path olamaz
    m.functions["store"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("file::store() requires file and subdir");

        // file_array doğrula
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("file::store(): first argument must be a file from request::file()");

        auto& arr = *args[0].as_array();
        bool is_assoc = !arr.empty() && arr[0].to_string() == "__assoc__";
        if (!is_assoc)
            throw std::runtime_error("file::store(): invalid file argument");

        // Dosya bilgilerini çıkar
        std::string temp_path, mime, sha256;
        for (size_t i = 1; i + 1 < arr.size(); i += 2) {
            std::string key = arr[i].to_string();
            if (key == "path")   temp_path = arr[i+1].to_string();
            if (key == "mime")   mime      = arr[i+1].to_string();
            if (key == "sha256") sha256    = arr[i+1].to_string();
        }
        if (temp_path.empty()) throw std::runtime_error("file::store(): missing temp path");

        // Alt klasör adı — path karakteri içeremez
        std::string subdir = args[1].to_string();
        if (subdir.find('/') != std::string::npos ||
            subdir.find('\\') != std::string::npos ||
            subdir.find("..") != std::string::npos)
            throw std::runtime_error("file::store(): subdir must be a simple name, not a path");

        // Hedef klasörü oluştur
        std::string upload_root = get_upload_root();
        fs::path dest_dir = fs::path(upload_root) / subdir;

        // Web root altına yazma güvenlik kontrolü
        // UPLOAD_DIR açıkça set edilmişse (allow_mime zaten MIME doğrulaması yaptı) geç
        bool upload_dir_explicit = (std::getenv("UPLOAD_DIR") && *std::getenv("UPLOAD_DIR"));
        if (!upload_dir_explicit && is_under_web_root(dest_dir.string()))
            throw std::runtime_error("file::store(): upload dir cannot be under web root — set UPLOAD_DIR to a path outside web root");

        fs::create_directories(dest_dir);

        // Uzantıyı MIME'dan belirle (temp dosyada zaten doğru uzantı var)
        fs::path tmp = fs::path(temp_path);
        std::string ext = tmp.extension().string();

        // sha256'nın ilk 32 karakterini dosya adı olarak kullan
        std::string safe_name = sha256.substr(0, 32) + ext;
        fs::path dest = dest_dir / safe_name;

        // Taşı (aynı dosya zaten varsa — deduplication)
        if (!fs::exists(dest))
            fs::rename(temp_path, dest);
        else
            fs::remove(temp_path);  // duplicate — temp sil

        // Public URL
        std::string base_url = get_upload_url();
        std::string url;
        if (!base_url.empty()) {
            if (base_url.back() != '/') base_url += '/';
            url = base_url + subdir + "/" + safe_name;
        }

        // Dönüş değeri
        auto result = std::make_shared<std::vector<Value>>();
        result->push_back(Value(std::string("__assoc__")));
        result->push_back(Value(std::string("path")));   result->push_back(Value(dest.string()));
        result->push_back(Value(std::string("name")));   result->push_back(Value(safe_name));
        result->push_back(Value(std::string("url")));    result->push_back(Value(url));
        result->push_back(Value(std::string("sha256"))); result->push_back(Value(sha256));
        result->push_back(Value(std::string("mime")));   result->push_back(Value(mime));
        return Value(result);
    };

    // file::upload_dir() → upload root string (bilgi amaçlı)
    m.functions["upload_dir"] = [](auto) -> Value {
        return Value(get_upload_root());
    };

    return m;
}

} // namespace look

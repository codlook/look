#include "look/stdlib.h"
#include "look/web.h"
#include "look/request_scope.h"   // current_request_web: isteğin DOCUMENT_ROOT değeri
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
// Süreç umask'ı — statik başlatmada (main'den önce, tek iş parçacığı) bir kez okunur: umask()
// okuma-yazma birlikte yapar, istek sırasında çağırmak başka iş parçacığının dosyasını etkilerdi.
static const mode_t g_process_umask = [] { mode_t m = ::umask(0); ::umask(m); return m; }();
#endif

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

// Web root tespiti — isteğin DOCUMENT_ROOT'u (FastCGI parametresi / CGI ortamı), yoksa süreç ortamı.
// FastCGI'de istek parametreleri artık süreç ortamına yazılmıyor (fcgi_main.cpp): değer isteğin
// bağlamından okunur, yoksa bu denetim FastCGI'de sessizce devre dışı kalırdı.
static std::string get_web_root() {
    if (auto* w = look::current_request_web(); w && !w->document_root.empty()) return w->document_root;
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

// ── Path traversal guard — GÜVENLİ VARSAYILAN ────────────────────────────────
// file:: işlemleri bir dizin ağacına kısıtlanır (kullanıcı girdisiyle
// `../../etc/passwd` okunmasını engeller). "PHP gibi dağıtılan ama Go gibi
// düşünen" bir dilde varsayılan güvenli olmalı:
//   • LOOK_FILE_ROOT set DEĞİLSE → çalışma dizinine (cwd) kısıtla (güvenli).
//   • LOOK_FILE_ROOT=<yol>       → o dizine kısıtla (genişletme, opt-in).
//   • LOOK_FILE_ROOT=*           → kısıtsız (açık opt-out; tüm sorumluluk sizde).
// (İç modüller — session/cache/jobs — file:: kullanmaz, bu kısıttan etkilenmez.)
static std::string get_file_root() {
    static std::string root = []() -> std::string {
        const char* r = std::getenv("LOOK_FILE_ROOT");
        if (r && std::string(r) == "*") return "";     // açık kısıtsız
        std::string base;
        if (r && *r) base = r;                          // kullanıcı kökü
        else {                                          // VARSAYILAN: cwd
            std::error_code ec;
            base = fs::current_path(ec).string();
            if (ec) return "";                          // cwd alınamadı → kısıtsız
        }
        std::error_code ec;
        auto p = fs::weakly_canonical(fs::path(base), ec);
        return ec ? base : p.string();
    }();
    return root;
}

// Yolu bileşen bileşen çöz; karşılaşılan HER symlink izlenir — hedefi var olmasa bile (Go 1.24
// os.Root modeli: kök dışını gösteren bağlantı hiçbir koşulda izlenmez). ESKİ AÇIK: weakly_canonical
// var olmayan hedefi çözemediği için sarkan bir bağlantıyı ("dangle" → /dışarı/yeni.txt) kök içinde
// sanıyordu → file::put kök DIŞINDA dosya yaratıyordu (ölçüldü). ".." bağlantı izlendikten sonra
// uygulanır (POSIX çözümüyle aynı). 40 bağlantı sınırı (Linux ELOOP).
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
    // Yol ÖNCE cwd-tabanlı mutlağa çevrilir (göreli "out.txt" cwd'ye göre — OS ile aynı çözüm),
    // sonra her bileşen ve her symlink (sarkan dahil) çözülür.
    fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) throw std::runtime_error("file: invalid path: " + path);
    fs::path resolved = resolve_all_links(abs);
    // Kökle AYNI biçime getir (kök weakly_canonical ile hesaplandı): Windows'ta sürücü harfi/büyük-küçük
    // harf farkı ("c:" ↔ "C:") meşru erişimi reddettirirdi. resolved'da artık bağlantı yok, güvenli.
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
            rec->push_back(Value(std::string("__assoc__")));
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

        // Yalnız BU isteğin request::file() ile aldığı geçici dosya saklanır; yol, sha256 ve mime
        // LOOK'un kendi kaydından alınır, çağıranın dizisinden DEĞİL. ESKİ AÇIK: dizi elle kurulursa
        // "path" sunucudaki herhangi bir dosyayı (.env) yükleme klasörüne taşıyıp UPLOAD_URL'den
        // sunduruyordu; "sha256" alanı dosya adına girdiği için "../../" ile klasör dışına yazılıyordu.
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

        // Uzantıyı MIME'dan belirle (temp dosyada zaten doğru uzantı var)
        fs::path tmp = fs::path(temp_path);
        std::string ext = tmp.extension().string();

        // sha256'nın ilk 32 karakterini dosya adı olarak kullan
        std::string safe_name = sha256.substr(0, 32) + ext;
        fs::path dest = dest_dir / safe_name;

        // Taşı (aynı dosya zaten varsa — deduplication)
        if (!fs::exists(dest)) {
            fs::rename(temp_path, dest);
#ifndef _WIN32
            // Geçici yükleme dosyası 0600 açılır (paylaşımlı /tmp'de başkası okumasın). Saklanan
            // dosya ise UPLOAD_URL ile web sunucusunca sunulur: normal dosya izni (0666 & ~umask).
            ::chmod(dest.c_str(), 0666 & ~g_process_umask);
#endif
        } else
            fs::remove(temp_path);  // duplicate — temp sil

        // Public URL
        std::string base_url = get_upload_url();
        std::string url;
        if (!base_url.empty()) {
            if (base_url.back() != '/') base_url += '/';
            url = base_url + subdir + "/" + safe_name;
        }

        // Dönüş değeri
        auto result = std::make_shared<ValueArray>();
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

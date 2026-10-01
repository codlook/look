#include "look/template.h"
#include "look/stdlib.h"
#include "look/html_escape.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>
#include <unordered_map>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace look {

// ──────────────────────────────────────────────────────────────────────────────
// Utilities
// ──────────────────────────────────────────────────────────────────────────────

static std::string tpl_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string TemplateEngine::html_escape(const std::string& s) {
    return look::html_escape(s);   // tek tanım: look/html_escape.h (member forwarder)
}

std::string TemplateEngine::to_str(const Value& v) {
    switch (v.type()) {
        case Value::INT:    return std::to_string(v.as_int());
        // BUG FIX: buradaki elle-yazılmış float biçimlemesi dilden SAPIYORDU. Tam-sayı
        // olmayan büyük değerlerde `oss << d` varsayılan 6 anlamlı basamağa düşüp
        // BİLİMSEL gösterime geçiyordu: 1234567.5 → "1.23457e+06" (dil: "1234567.5").
        // Yani şablonda basılan fiyat/toplam hem okunamaz hem YANLIŞ (1.23457e+06 =
        // 1234570). Dilin biçimleyicisine delege et — tek kaynak, ayrışma imkânsız.
        case Value::FLOAT: return look_format_double(v.as_float());
        case Value::BOOL:   return v.as_bool() ? "true" : "false";
        case Value::STRING: return v.as_string();
        default:            return "";
    }
}

bool TemplateEngine::is_truthy(const Value& v) {
    // BUG FIX: burada dilin truthiness'i KOPYALANMIŞTI ve bir kural atlanmıştı —
    // STRING dalı `!empty()` diyordu, oysa dil (Value::is_truthy) `!empty() && != "0"`
    // uygular. Sonuç: `{#if $x}` ile `if ($x)` string "0" için ZIT karar veriyordu
    // (DB'den string dönen stock="0" / active="0" alanları kodda falsy, şablonda truthy
    // → sessizce yanlış dal render ediliyordu). Kopya semantik = kaçınılmaz ayrışma;
    // tek kaynağa delege et ki dilin truthiness'i değişirse şablon otomatik hizalansın.
    return v.is_truthy();
}

// Resolve "varname", "varname.field", "varname.field.sub"
Value TemplateEngine::resolve(const std::string& path, const TplContext& ctx) {
    if (path.empty()) return Value();

    // Split by '.'
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else           cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return Value();

    auto it = ctx.find(parts[0]);
    if (it == ctx.end()) return Value();
    Value v = it->second;

    for (size_t i = 1; i < parts.size(); ++i) {
        if (!v.as_array()) return Value();
        const auto& arr = *v.as_array();
        const std::string& key = parts[i];

        // Assoc array: [sentinel, k0, v0, k1, v1, ...]
        // Sentinel is a special string "__assoc__" or "__struct__" at index 0
        size_t start = 0;
        bool   is_assoc = !arr.empty() &&
                          arr[0].type() == Value::STRING &&
                          (arr[0].as_string() == "__assoc__" ||
                           arr[0].as_string() == "__struct__");
        if (is_assoc) start = 1;

        // Try key lookup in k/v pairs
        bool found = false;
        for (size_t j = start; j + 1 < arr.size(); j += 2) {
            if (arr[j].type() == Value::STRING && arr[j].as_string() == key) {
                v = arr[j + 1];
                found = true;
                break;
            }
        }
        if (!found) {
            // Try numeric index in plain array
            bool is_num = !key.empty() &&
                          std::all_of(key.begin(), key.end(), ::isdigit);
            if (is_num) {
                int idx = std::stoi(key);
                if (idx >= 0 && (size_t)idx < arr.size()) { v = arr[idx]; found = true; }
            }
        }
        if (!found) return Value();
    }
    return v;
}

// ──────────────────────────────────────────────────────────────────────────────
// Condition evaluation
// Supports: $var, !$var, $var == "x", $var != "x", $var > 0, etc.
// ──────────────────────────────────────────────────────────────────────────────

static Value parse_rhs(const std::string& rhs, const TplContext& ctx) {
    std::string r = tpl_trim(rhs);
    if (r.size() >= 2 && r.front() == '"' && r.back() == '"')
        return Value(r.substr(1, r.size() - 2));
    if (!r.empty() && r[0] == '$')
        return TemplateEngine::resolve(r.substr(1), ctx);
    try { return Value((int64_t)std::stoll(r)); } catch (...) {}
    try { return Value(std::stod(r)); }        catch (...) {}
    return Value(r);
}

bool TemplateEngine::eval_cond(const std::string& cond, const TplContext& ctx) {
    std::string s = tpl_trim(cond);

    bool negate = false;
    if (!s.empty() && s[0] == '!') {
        negate = true;
        s = tpl_trim(s.substr(1));
    }
    // Strip leading $
    if (!s.empty() && s[0] == '$') s = s.substr(1);

    // Look for comparison operators (order matters: >= before >, etc.)
    static const char* ops[] = { ">=", "<=", "!=", "==", ">", "<", nullptr };
    for (int i = 0; ops[i]; ++i) {
        std::string op = ops[i];
        size_t pos = s.find(op);
        if (pos == std::string::npos) continue;

        std::string lhs_str = tpl_trim(s.substr(0, pos));
        std::string rhs_str = tpl_trim(s.substr(pos + op.size()));

        if (!lhs_str.empty() && lhs_str[0] == '$') lhs_str = lhs_str.substr(1);

        Value lv = resolve(lhs_str, ctx);
        Value rv = parse_rhs(rhs_str, ctx);

        bool result;
        if (op == "==" || op == "!=") {
            bool eq = (to_str(lv) == to_str(rv));
            result = (op == "==") ? eq : !eq;
        } else {
            auto to_dbl = [](const Value& v) -> double {
                if (v.type() == Value::INT)   return (double)v.as_int();
                if (v.type() == Value::FLOAT) return v.as_float();
                try { return std::stod(TemplateEngine::to_str(v)); } catch (...) { return 0.0; }
            };
            double ld = to_dbl(lv), rd = to_dbl(rv);
            if      (op == ">")  result = ld > rd;
            else if (op == "<")  result = ld < rd;
            else if (op == ">=") result = ld >= rd;
            else                 result = ld <= rd;
        }
        return negate ? !result : result;
    }

    // Simple truthy check
    Value v = resolve(s, ctx);
    return negate ? !is_truthy(v) : is_truthy(v);
}

// ──────────────────────────────────────────────────────────────────────────────
// Parser
// ──────────────────────────────────────────────────────────────────────────────

struct TplParser {
    const std::string& src;
    size_t             pos = 0;
    std::string        origin;

    TplParser(const std::string& s, const std::string& o) : src(s), pos(0), origin(o) {}

    bool at_end() const { return pos >= src.size(); }

    // Read characters until '}', consuming the '}'
    std::string read_until_close(const std::string& ctx_msg) {
        size_t start = pos;
        while (pos < src.size() && src[pos] != '}') ++pos;
        if (pos >= src.size())
            throw std::runtime_error("Template parse error in '" + origin +
                                     "': unclosed '{' near: " + ctx_msg);
        std::string content = src.substr(start, pos - start);
        ++pos; // consume '}'
        return content;
    }

    // Validate a variable path — no expressions or function calls allowed
    void validate_var_path(const std::string& path) {
        for (char c : path) {
            if (c == '(' || c == ')' || c == '+' || c == '*' || c == '%' ||
                c == '/' || c == '!' || c == '&' || c == '|' || c == '?') {
                throw std::runtime_error(
                    "Template error: '{$" + path + "}' cannot contain an expression or function "
                    "call cannot be used. Do the computation in the .lk file.");
            }
            // Disallow :: (module calls)
            if (c == ':' && path.find("::") != std::string::npos) {
                throw std::runtime_error(
                    "Template error: '{$" + path + "}' cannot contain a module call.");
            }
        }
    }

    // Try to read a template block at current position.
    // Returns true and sets kind/args if this is a template directive.
    // Returns false if '{' is literal text.
    // kind: "var", "rawvar", "#if", "#else", "#each", "#empty",
    //       "#extends", "#block", "#include", "/if", "/each", "/block"
    bool try_block(std::string& kind, std::string& args) {
        if (pos >= src.size() || src[pos] != '{') return false;
        size_t save = pos;
        ++pos; // consume '{'

        if (pos >= src.size()) { pos = save; return false; }
        char c = src[pos];

        // {!$var} — raw (unescaped) variable
        if (c == '!' && pos + 1 < src.size() && src[pos+1] == '$') {
            pos += 2; // consume '!$'
            std::string path = tpl_trim(read_until_close("!$..."));
            validate_var_path(path);
            kind = "rawvar";
            args = path;
            return true;
        }

        // {$var} — escaped variable
        if (c == '$') {
            ++pos; // consume '$'
            std::string path = tpl_trim(read_until_close("$..."));
            validate_var_path(path);
            kind = "var";
            args = path;
            return true;
        }

        // {#directive args} — block start
        if (c == '#') {
            ++pos; // consume '#'
            std::string inner = tpl_trim(read_until_close("#..."));
            auto sp = inner.find(' ');
            std::string directive = sp == std::string::npos ? inner : inner.substr(0, sp);
            args = sp == std::string::npos ? "" : tpl_trim(inner.substr(sp + 1));
            kind = "#" + directive;
            return true;
        }

        // {/directive} — block end
        if (c == '/') {
            ++pos; // consume '/'
            std::string inner = tpl_trim(read_until_close("/..."));
            kind = "/" + inner;
            args = "";
            return true;
        }

        // Not a template directive — restore and treat as literal
        pos = save;
        return false;
    }

    // Parse nodes until EOF or until a directive in `stops` is found.
    // Returns the stop directive encountered, or "" for EOF.
    std::string parse_nodes(std::vector<TplNode>& out,
                             const std::vector<std::string>& stops = {}) {
        std::string text_buf;

        auto flush_text = [&]() {
            if (!text_buf.empty()) {
                out.push_back({TplNodeKind::Text, text_buf});
                text_buf.clear();
            }
        };

        while (!at_end()) {
            if (src[pos] != '{') {
                text_buf += src[pos++];
                continue;
            }

            std::string kind, args;
            if (!try_block(kind, args)) {
                text_buf += src[pos++]; // literal '{'
                continue;
            }

            // Check if this is a stop directive
            for (const auto& stop : stops) {
                if (kind == stop) {
                    flush_text();
                    return kind;
                }
            }

            flush_text();

            if (kind == "var") {
                out.push_back({TplNodeKind::Var, args});
            }
            else if (kind == "rawvar") {
                out.push_back({TplNodeKind::RawVar, args});
            }
            else if (kind == "#if") {
                TplNode node;
                node.kind  = TplNodeKind::If;
                node.extra = args; // condition string
                std::string stop = parse_nodes(node.children, {"#else", "/if"});
                if (stop == "#else") {
                    parse_nodes(node.alt, {"/if"});
                }
                out.push_back(std::move(node));
            }
            else if (kind == "#each") {
                // expected: "$arr as $item"
                TplNode node;
                node.kind = TplNodeKind::Each;
                auto as_pos = args.find(" as ");
                if (as_pos == std::string::npos)
                    throw std::runtime_error(
                        "Template error (" + origin + "): {#each} syntax error. "
                        "Correct usage: {#each $list as $item}");
                std::string arr_part  = tpl_trim(args.substr(0, as_pos));
                std::string item_part = tpl_trim(args.substr(as_pos + 4));
                if (!arr_part.empty()  && arr_part[0]  == '$') arr_part  = arr_part.substr(1);
                if (!item_part.empty() && item_part[0] == '$') item_part = item_part.substr(1);
                node.text  = arr_part;   // array variable path
                node.extra = item_part;  // item variable name
                std::string stop = parse_nodes(node.children, {"#empty", "/each"});
                if (stop == "#empty") {
                    parse_nodes(node.alt, {"/each"});
                }
                out.push_back(std::move(node));
            }
            else if (kind == "#extends") {
                std::string path = args;
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                    path = path.substr(1, path.size() - 2);
                out.push_back({TplNodeKind::Extends, path});
            }
            else if (kind == "#block") {
                std::string name = args;
                if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                    name = name.substr(1, name.size() - 2);
                TplNode node;
                node.kind = TplNodeKind::Block;
                node.text = name;
                parse_nodes(node.children, {"/block"});
                out.push_back(std::move(node));
            }
            else if (kind == "#include") {
                // {#include "path"} or {#include "path" data=$var}
                std::string path_str = args;
                std::string data_var;
                auto data_pos = args.find(" data=");
                if (data_pos != std::string::npos) {
                    path_str = tpl_trim(args.substr(0, data_pos));
                    data_var = tpl_trim(args.substr(data_pos + 6));
                    if (!data_var.empty() && data_var[0] == '$')
                        data_var = data_var.substr(1);
                }
                if (path_str.size() >= 2 && path_str.front() == '"' && path_str.back() == '"')
                    path_str = path_str.substr(1, path_str.size() - 2);
                TplNode node;
                node.kind  = TplNodeKind::Include;
                node.text  = path_str;
                node.extra = data_var;
                out.push_back(std::move(node));
            }
            else if (kind[0] == '/' || kind == "#else" || kind == "#empty") {
                // Unexpected stop directive
                throw std::runtime_error(
                    "Template error (" + origin + "): unexpected directive '{" + kind + "}'");
            }
            else {
                throw std::runtime_error(
                    "Template error (" + origin + "): unknown directive '{" + kind + "}'");
            }
        }

        flush_text();
        return ""; // EOF
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// TemplateEngine public API
// ──────────────────────────────────────────────────────────────────────────────

std::vector<TplNode> TemplateEngine::parse(const std::string& src, const std::string& origin) {
    TplParser parser(src, origin);
    std::vector<TplNode> nodes;
    parser.parse_nodes(nodes);
    return nodes;
}

// Ayırıcı-sınırlı containment: f, b'nin kendisi VEYA altında mı. Düz string-prefix
// YETMEZ — "/proj" öneki "/proj-evil" ile de eşleşir (sibling-prefix escape).
// (installer zip-slip ve file:: ile aynı disiplin.)
static bool tpl_within(const std::string& b, const std::string& f) {
    return f.size() >= b.size() && f.compare(0, b.size(), b) == 0 &&
           (f.size() == b.size() || f[b.size()] == '/' || f[b.size()] == '\\');
}

std::string TemplateEngine::resolve_path(const std::string& path) {
    fs::path p(path);
    // Add .html extension if no extension given
    if (!p.has_extension()) p += ".html";

    // İzinli kökler: çalışma dizini + (varsa) TEMPLATE_DIR. Çözülen yol bunlardan
    // birinin ALTINDA olmalı — aksi halde template::render(user_input) ile
    // "../.." veya mutlak yol üzerinden arbitrary .html file read (içerik client'a
    // döner → bilgi sızıntısı). Mutlak yol da containment'tan muaf DEĞİL.
    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    const char* tdir = std::getenv("TEMPLATE_DIR");
    if (tdir && *tdir) roots.push_back(fs::path(tdir));

    for (const auto& root : roots) {
        fs::path cand = p.is_absolute() ? p : (root / p);
        std::string root_str = fs::weakly_canonical(root).string();
        std::string cand_str = fs::weakly_canonical(cand).string();
        if (tpl_within(root_str, cand_str) && fs::exists(cand))
            return cand.string();
    }

    // Hiçbir izinli kök altında bulunamadı. Yolun bir kök içinde KALIP kalmadığını
    // kontrol et: kalıyorsa "bulunamadı" (best-guess), çıkıyorsa güvenlik hatası.
    fs::path base = fs::current_path();
    std::string base_str = fs::weakly_canonical(base).string();
    std::string full_str = fs::weakly_canonical(base / p).string();
    if (!p.is_absolute() && tpl_within(base_str, full_str))
        return (base / p).string();   // kök içinde ama yok → açılışta net hata

    throw std::runtime_error("Template security error: escape outside the allowed directory blocked: " + path);
}

void TemplateEngine::collect_blocks(const std::vector<TplNode>& nodes, TplBlocks& out) {
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Block) {
            if (!out.count(n.text)) // child definition wins; don't overwrite
                out[n.text] = n.children;
        }
        collect_blocks(n.children, out);
        collect_blocks(n.alt,      out);
    }
}

// ── Ayrıştırılmış şablon önbelleği ───────────────────────────────────────────────
// Eskiden her render (ve her {#include} / {#extends}) dosyayı yeniden çözüyor, açıyor,
// okuyor ve ayrıştırıyordu: 23 dosyalı gerçekçi bir sayfa ~400 µs, tek dosyalı sayfa
// ~30 µs (cpp/bench/tpl/micro.lk) — maliyetin neredeyse tamamı dosya-başı sabit işti.
//
// • thread_local: kilit yok; her worker kendi önbelleğini tutar.
// • Anahtar = çalışma dizini + TEMPLATE_DIR + İSTENEN yol (resolve_path'in girdileri).
// • Her kullanımda mtime+boyut kontrolü → dosya değişince yeniden çözülür/ayrıştırılır:
//   HOT-RELOAD korunur ve resolve_path'in dizin-dışı-kaçış kontrolü her yenilemede tekrar
//   uygulanır. Yeni bir yol string'i ilk kullanımında her zaman resolve_path'ten geçer →
//   template::render(kullanıcı_girdisi) korumasında değişiklik yok.
// • mtime dosya OKUNMADAN ÖNCE alınır: okuma sırasında dosya değişirse eski mtime ile yeni
//   içerik saklanır → sonraki çağrı farkı görüp yeniler (tersi kalıcı bayat içerik olurdu).
// • 1024 girdi sınırı: yol kullanıcıdan geliyorsa ("views/./././a" gibi sonsuz eş-anlamlı
//   varyant) sınırsız önbellek bellek-tüketme saldırısı olurdu.
// • shared_ptr: render sürerken girdi yenilense bile kullanılan düğüm ağacı yaşar.
// • Değişiklik damgası = inode + mtime + boyut, POSIX'te TEK stat çağrısıyla. Linux dosya
//   zaman damgaları kaba saatle tutulur (birkaç ms): aynı boyutta içerik aynı dilimde iki
//   kez yazılırsa mtime+boyut ayırt edemez. Geçici-dosya + rename ile yapılan dağıtımda
//   inode değişir → yakalanır. (Windows'ta inode yok: mtime + boyut.)
namespace {
struct TplStamp {
    bool               ok    = false;
    std::uintmax_t     size  = 0;
    long long          mtime = 0;
    unsigned long long ino   = 0;
    unsigned long long dev   = 0;
    bool operator==(const TplStamp& o) const {
        return ok && o.ok && size == o.size && mtime == o.mtime && ino == o.ino && dev == o.dev;
    }
};
TplStamp tpl_stamp(const std::string& full) {
    TplStamp s;
#ifdef _WIN32
    std::error_code ec;
    auto mt = fs::last_write_time(full, ec);
    if (ec) return s;
    auto sz = fs::file_size(full, ec);
    if (ec) return s;
    s.ok = true; s.size = sz; s.mtime = (long long)mt.time_since_epoch().count();
#else
    struct stat st;
    if (::stat(full.c_str(), &st) != 0) return s;
    s.ok    = true;
    s.size  = (std::uintmax_t)st.st_size;
    s.mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + (long long)st.st_mtim.tv_nsec;
    s.ino   = (unsigned long long)st.st_ino;
    s.dev   = (unsigned long long)st.st_dev;
#endif
    return s;
}
struct TplCacheEntry {
    std::string                                   full;
    TplStamp                                      stamp;
    std::shared_ptr<const std::vector<TplNode>>   nodes;
    unsigned long long                            checked_epoch = 0;
};

// Üst-seviye render kapsamı. Tek bir sayfa render'ı boyunca (a) her dosya EN FAZLA BİR KEZ
// doğrulanır — 20 kez {#include} edilen bir partial eskiden aynı sayfada 20 kez stat'lanıyordu;
// ayrıca bir sayfa aynı partial'ın iki farklı sürümünü karıştırarak üretilmemeli — ve (b)
// çalışma dizini + TEMPLATE_DIR bir kez okunur. İkisi süreç boyunca sabit VARSAYILMAZ (FastCGI
// modunda istek başına değişebilirler); yalnız tek render içinde değişmezler.
//
// FIBER GÜVENLİĞİ: kapsam thread-local DEĞİL, çağrı zinciri boyunca parametre olarak taşınır.
// Bugün render hiç beklemeye geçmez (dosya okuma bloklayan çağrılarla, şablondan DB/I-O yok),
// ama bunu varsayıma bırakmıyoruz: thread-local bir "şu anki render" durumu, render'a ileride
// yield eden bir çağrı eklenirse aynı thread'deki iki fiber'ın birbirinin çalışma dizinini/
// doğrulama kümesini ezmesine (siteler-arası şablon sızıntısı) yol açardı. Thread-local kalan
// tek şey epoch SAYACI (yalnız benzersiz numara üretir) ve önbelleğin kendisidir; önbelleğe
// erişim tek bir load_nodes çağrısı içinde başlar ve biter, arada bekleme noktası yoktur.
struct TplEnv {
    unsigned long long epoch = 0;
    std::string        prefix;   // çalışma dizini + '\n' + TEMPLATE_DIR + '\n'
};
TplEnv tpl_make_env() {
    thread_local unsigned long long counter = 0;
    TplEnv env;
    env.epoch = ++counter;
    std::error_code ec;
    env.prefix = fs::current_path(ec).string();
    env.prefix += '\n';
    if (const char* t = std::getenv("TEMPLATE_DIR")) env.prefix += t;
    env.prefix += '\n';
    return env;
}
}

static std::shared_ptr<const std::vector<TplNode>> load_nodes(const std::string& path,
                                                               const TplEnv& env) {
    thread_local std::unordered_map<std::string, TplCacheEntry> cache;
    const unsigned long long epoch = env.epoch;
    std::string key = env.prefix;
    key += path;

    auto it = cache.find(key);
    if (it != cache.end()) {
        if (it->second.checked_epoch == epoch)          // bu render'da zaten doğrulandı
            return it->second.nodes;
        if (tpl_stamp(it->second.full) == it->second.stamp) {
            it->second.checked_epoch = epoch;
            return it->second.nodes;
        }
        cache.erase(it);   // değişti ya da kayboldu → baştan
    }

    TplCacheEntry e;
    e.full  = TemplateEngine::resolve_path(path);
    e.stamp = tpl_stamp(e.full);          // OKUMADAN ÖNCE (yukarıdaki nota bakın)
    e.checked_epoch = epoch;
    std::ifstream f(e.full, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("Template file not found: " + e.full);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    e.nodes = std::make_shared<const std::vector<TplNode>>(TemplateEngine::parse(src, path));
    auto nodes = e.nodes;
    if (e.stamp.ok) {                 // stat alınamadıysa önbelleğe koyma (her seferinde oku)
        if (cache.size() >= 1024) cache.clear();
        cache[std::move(key)] = std::move(e);
    }
    return nodes;
}

// ── Render ───────────────────────────────────────────────────────────────────────
// Tek çıktı tamponu: iç bloklar (if/each/block/include) eskiden ayrı bir string üretip
// üstteki string'e kopyalıyordu (derinlik × boyut kadar kopya); artık hepsi aynı `out`'a
// yazar. {#extends} blokları da KOPYALANMAZ — düğüm ağaçları önbellekte yaşadığı için
// işaretçiyle taşınır (eskiden her istekte blokların alt ağaçları derin kopyalanıyordu).
using TplBlockPtrs = std::map<std::string, const std::vector<TplNode>*>;

static void collect_block_ptrs(const std::vector<TplNode>& nodes, TplBlockPtrs& out) {
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Block) {
            if (!out.count(n.text)) // child definition wins; don't overwrite
                out[n.text] = &n.children;
        }
        collect_block_ptrs(n.children, out);
        collect_block_ptrs(n.alt,      out);
    }
}

static void render_into(const std::vector<TplNode>& nodes, const TplContext& ctx,
                        const TplBlockPtrs* blocks, std::string& out, const TplEnv& env);

// {#extends} varsa: çocuğun bloklarını topla, ebeveyni render et. `nodes` çağıranın elinde
// yaşamalı (blok işaretçileri onun içine bakar). Ebeveynin kendi {#extends}'i izlenmez —
// tek seviye kalıtım, önceki davranışla aynı.
static bool render_extends(const std::vector<TplNode>& nodes, const TplContext& ctx,
                           std::string& out, const TplEnv& env) {
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Text) continue; // whitespace before extends is OK
        if (n.kind == TplNodeKind::Extends) {
            TplBlockPtrs blocks;
            collect_block_ptrs(nodes, blocks);
            auto parent = load_nodes(n.text, env);
            render_into(*parent, ctx, &blocks, out, env);
            return true;
        }
        break; // non-extends first real node → not a child template
    }
    return false;
}

static void render_file_into(const std::string& path, const TplContext& ctx, std::string& out, const TplEnv& env) {
    auto holder = load_nodes(path, env);
    if (!render_extends(*holder, ctx, out, env))
        render_into(*holder, ctx, nullptr, out, env);
}

std::string TemplateEngine::render_file(const std::string& path, const TplContext& ctx) {
    const TplEnv env = tpl_make_env();
    std::string out;
    render_file_into(path, ctx, out, env);
    return out;
}

std::string TemplateEngine::render_string(const std::string& src, const TplContext& ctx) {
    const TplEnv env = tpl_make_env();
    auto nodes = parse(src, "<string>");
    std::string out;
    if (!render_extends(nodes, ctx, out, env))
        render_into(nodes, ctx, nullptr, out, env);
    return out;
}

std::string TemplateEngine::render(const std::vector<TplNode>& nodes,
                                    const TplContext& ctx,
                                    const TplBlocks* blocks) {
    const TplEnv env = tpl_make_env();
    std::string out;
    if (blocks) {
        TplBlockPtrs ptrs;
        for (const auto& kv : *blocks) ptrs[kv.first] = &kv.second;
        render_into(nodes, ctx, &ptrs, out, env);
    } else {
        render_into(nodes, ctx, nullptr, out, env);
    }
    return out;
}

static void render_into(const std::vector<TplNode>& nodes, const TplContext& ctx,
                        const TplBlockPtrs* blocks, std::string& out, const TplEnv& env) {
    for (const auto& n : nodes) {
        switch (n.kind) {
            case TplNodeKind::Text:
                out += n.text;
                break;

            case TplNodeKind::Var: {
                Value v = TemplateEngine::resolve(n.text, ctx);
                out += TemplateEngine::html_escape(TemplateEngine::to_str(v));
                break;
            }

            case TplNodeKind::RawVar: {
                Value v = TemplateEngine::resolve(n.text, ctx);
                out += TemplateEngine::to_str(v);
                break;
            }

            case TplNodeKind::If:
                if (TemplateEngine::eval_cond(n.extra, ctx))
                    render_into(n.children, ctx, blocks, out, env);
                else
                    render_into(n.alt,      ctx, blocks, out, env);
                break;

            case TplNodeKind::Each: {
                Value arr_val = TemplateEngine::resolve(n.text, ctx);
                if (arr_val.type() != Value::ARRAY || !arr_val.as_array()) {
                    render_into(n.alt, ctx, blocks, out, env); // empty branch
                    break;
                }
                const auto& arr = *arr_val.as_array();

                // Determine whether to skip sentinel at index 0
                bool is_assoc = !arr.empty() &&
                                arr[0].type() == Value::STRING &&
                                (arr[0].as_string() == "__assoc__" ||
                                 arr[0].as_string() == "__struct__");

                std::vector<const Value*> elems;
                if (is_assoc) {
                    // Assoc: [sentinel, k0, v0, k1, v1, ...] → iterate values
                    for (size_t j = 2; j < arr.size(); j += 2)
                        elems.push_back(&arr[j]);
                } else {
                    for (const auto& e : arr)
                        elems.push_back(&e);
                }

                if (elems.empty()) {
                    render_into(n.alt, ctx, blocks, out, env);
                    break;
                }
                // Context döngü başına BİR kez kopyalanır; her yinelemede yalnız döngü
                // değişkeni yeniden atanır (eskiden tüm harita her yinelemede kopyalanıyordu).
                // Gövde ctx'i const görür → yinelemeler arası sızıntı yok.
                TplContext child = ctx;
                Value& slot = child[n.extra];
                for (const Value* elem : elems) {
                    slot = *elem;
                    render_into(n.children, child, blocks, out, env);
                }
                break;
            }

            case TplNodeKind::Extends:
                // Handled in render_extends before this call
                break;

            case TplNodeKind::Block: {
                if (blocks) {
                    auto it = blocks->find(n.text);
                    if (it != blocks->end()) {
                        render_into(*it->second, ctx, blocks, out, env);
                        break;
                    }
                }
                // No override → use default block content
                render_into(n.children, ctx, blocks, out, env);
                break;
            }

            case TplNodeKind::Include: {
                // Build include context
                TplContext inc_ctx;
                const TplContext* use_ctx = &ctx;   // inherit parent context (kopyasız)
                if (!n.extra.empty()) {
                    // data=$var → use that var as context (assoc array → k/v pairs)
                    Value data = TemplateEngine::resolve(n.extra, ctx);
                    if (data.type() == Value::ARRAY && data.as_array()) {
                        const auto& arr = *data.as_array();
                        size_t start = 0;
                        if (!arr.empty() &&
                            arr[0].type() == Value::STRING &&
                            (arr[0].as_string() == "__assoc__" ||
                             arr[0].as_string() == "__struct__"))
                            start = 1;
                        for (size_t j = start; j + 1 < arr.size(); j += 2) {
                            if (arr[j].type() == Value::STRING)
                                inc_ctx[arr[j].as_string()] = arr[j+1];
                        }
                    }
                    use_ctx = &inc_ctx;
                }
                try {
                    render_file_into(n.text, *use_ctx, out, env);
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        std::string("Template include error (") + n.text + "): " + e.what());
                }
                break;
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// LOOK module: use template;
// ──────────────────────────────────────────────────────────────────────────────

static TplContext value_to_context(const Value& v) {
    TplContext ctx;
    if (v.type() != Value::ARRAY || !v.as_array()) return ctx;
    const auto& arr = *v.as_array();
    size_t start = 0;
    if (!arr.empty() &&
        arr[0].type() == Value::STRING &&
        (arr[0].as_string() == "__assoc__" || arr[0].as_string() == "__struct__"))
        start = 1;
    for (size_t i = start; i + 1 < arr.size(); i += 2) {
        if (arr[i].type() == Value::STRING)
            ctx[arr[i].as_string()] = arr[i+1];
    }
    return ctx;
}

Module make_template_module(Interpreter* interp) {
    Module m;
    m.name = "template";

    // template::render("views/home", $data) → string
    // Returns rendered HTML. Use print(template::render(...)) in scripts.
    m.functions["render"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("template::render() expects 1 or 2 arguments: (file_path [, $data])");
        if (args[0].type() != Value::STRING)
            throw std::runtime_error("template::render(): first argument must be a string (file path)");
        TplContext ctx;
        if (args.size() == 2) ctx = value_to_context(args[1]);
        std::string html = TemplateEngine::render_file(args[0].as_string(), ctx);
        return Value(html);
    };

    // template::render_string("<h1>{$title}</h1>", $data) → string
    m.functions["render_string"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("template::render_string() expects 1 or 2 arguments");
        if (args[0].type() != Value::STRING)
            throw std::runtime_error("template::render_string(): first argument must be a string");
        TplContext ctx;
        if (args.size() == 2) ctx = value_to_context(args[1]);
        std::string html = TemplateEngine::render_string(args[0].as_string(), ctx);
        return Value(html);
    };

    // template::escape($str) → HTML-escaped string (for manual use)
    m.functions["escape"] = [](std::vector<Value> args) -> Value {
        if (args.size() != 1)
            throw std::runtime_error("template::escape() expects 1 argument");
        std::string s = args[0].type() == Value::STRING
                        ? args[0].as_string()
                        : TemplateEngine::to_str(args[0]);
        return Value(TemplateEngine::html_escape(s));
    };

    return m;
}

} // namespace look

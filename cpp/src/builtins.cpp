// builtins.cpp — VM builtin tablosunun tek kaynağı (bkz. builtins.h)

#include "look/builtins.h"

#include <unordered_map>
#include <unordered_set>

namespace look {

const std::vector<std::string>& builtin_names() {
    static const std::vector<std::string> NAMES = {
        // 0-9: core
        "print", "write", "count", "push", "pop", "str", "int", "float", "bool", "string",
        // 10-15: type check
        "is_null", "is_int", "is_float", "is_string", "is_bool", "is_array",
        // 16-19: channel (special opcodes — CHAN_SEND/RECV — ama fallback için listede)
        "send", "receive", "close", "chan_size",
        // 20-21: json (eski isimlendirme — geriye dönük uyumluluk)
        "json_encode", "json_decode",
        // 22: route — setup'ta closure kaydeder, dispatch'te no-op
        "route",
        // 23-24: json module
        "json::encode", "json::decode",
        // 25-27: response
        "response::status", "response::header", "response::redirect",
        // 28-36: request
        "request::method", "request::get", "request::post", "request::json",
        "request::body", "request::path", "request::ip", "request::param",
        "request::all",
        // 37-42: db
        "db::query", "db::exec", "db::last_id", "db::affected", "db::col", "db::close",
        // 43-47: log
        "log::info", "log::warn", "log::error", "log::debug", "log::query",
        // 48-51: session
        "session::start", "session::get", "session::set", "session::destroy", "session::regenerate",
        // 52-55: cookie
        "cookie::get", "cookie::set", "cookie::delete", "cookie::has",
        // 56: db::connect (setup'ta çalışır — pool oluşturur, key string döner)
        "db::connect",
        // 57: channel() — LookChannel oluşturur
        "channel",
        // 58: env() — setup'ta da çağrılır (DB DSN oluşturma)
        "env",
        // 59: config()
        "config",
        // 60-69: date::
        "date::now", "date::today", "date::format", "date::parse", "date::add",
        "date::sub", "date::diff", "date::from_timestamp", "date::is_valid", "date::weekday",
        // 70-89: string::
        "string::upper", "string::lower", "string::trim", "string::replace", "string::contains",
        "string::substr", "string::split", "string::join", "string::len", "string::starts_with",
        "string::ends_with", "string::reverse", "string::repeat", "string::index_of",
        "string::pad_left", "string::pad_right", "string::slugify", "string::random",
        "string::ltrim", "string::rtrim",
        // 90-91: auth::
        "auth::hash", "auth::verify",
        // 92-97: validator::
        "validator::required", "validator::email", "validator::integer",
        "validator::numeric", "validator::min", "validator::max",
        // 98-100: html::
        "html::escape", "html::attr", "html::strip",
        // 101-103: math::
        "math::abs", "math::floor", "math::ceil",
        // 104-105: date:: devam
        "date::week", "date::timestamp",
        // 106-112: file::
        "file::read", "file::put", "file::append", "file::exists",
        "file::remove", "file::size", "file::store",
        // 113-119: http:: client
        "http::get", "http::post", "http::put", "http::delete",
        "http::patch",   // http::head/request HAYALET KALDIRILDI (impl yok; get/post/... kapsar)
        // 120-123: template::
        "template::render", "template::include", "template::block", "template::extends",
        // 124-128: cache::
        "cache::get", "cache::set", "cache::has", "cache::delete", "cache::flush",
        // 129-132: queue::
        "queue::push", "queue::pop", "queue::size",   // queue::flush HAYALET KALDIRILDI (gerçeği queue::clear)
        // 133-134: mail::
        "mail::send", "mail::deliver_maildir",
        // crypto:: — HAYALET KALDIRILDI (docs-conformance/ölçüm): sha512/md5/hmac/random_hex
        // builtin listesindeydi ama crypto modülünde implementasyonu YOK (çağrılınca "no function").
        // Gerçek karşılıkları: sha256 var; hmac→hmac_sha256; random_hex→random_bytes+hex_encode.
        // base64::encode/decode de HAYALET'ti (base64 modülü YOK → json modülüyle karışıyordu);
        // gerçek base64 crypto::base64_encode/decode. uuid::v4 HAYALET → gerçeği crypto::uuid.
        "crypto::sha256", "crypto::random_bytes",
        // 144-147: parallel:: observability
        "parallel::active", "parallel::wait", "parallel::limit", "parallel::at_capacity",
        // 148-151: error::
        "error::new", "error::is", "error::message", "error::code",
        // 152: response::json
        "response::json",
        // 153: before_route() — global middleware kayıt
        "before_route",
        // 154: stop() — middleware'den route execution'ı iptal et
        "stop",
        // 155-161: skaler core builtins (interpreter'da inline)
        "strlen", "abs", "max", "min", "sqrt", "strtoupper", "strtolower",
        // 162-163: request:: gelen header okuma (headers_in'den)
        "request::header", "request::headers",
        // 164: response::error — durum kodu + hata JSON (tek satir)
        "response::error",
        // 165-168: app:: — servis kaydi (app::set/get/has/db)
        "app::set", "app::get", "app::has", "app::db",
        // 169-170: response:: govde yardimcilari (VM-native dispatch)
        "response::text", "response::html",
        // 171+: array:: modulu — eskiden builtin_names'de HIC yoktu → route icinde
        // array::map/keys/sort vb. cagirinca compiler CALL_BUILTIN uretemeyip genel
        // CALL'a dusuyor, runtime'da "Not callable" firlatiyor ve route KALICI
        // interpreter'a dusuyordu (veri-agirlikli route'lar tamamen yavas yolda).
        // Sona eklendi (mevcut indexler kaymaz); `use array` ile auto-wire olur (satir ~905),
        // interpreter ile parity (o da `use array` gerektirir).
        "array::map", "array::filter", "array::reduce", "array::find", "array::keys",
        "array::values", "array::sort", "array::reverse", "array::slice", "array::contains",
        "array::unique", "array::flatten", "array::chunk", "array::zip", "array::all",
        "array::any", "array::push", "array::pop", "array::set", "array::new_assoc",
        // NOT: array::random_bytes/random_string/hex_encode/hex_decode/constant_compare/uuid
        // KALDIRILDI (docs-conformance taraması buldu) — bunlar crypto:: fonksiyonlarının
        // array:: namespace'ine yanlış kopyasıydı; array modülünde karşılıkları YOK →
        // çağrılınca "'array' has no function" / "Module not loaded" (fantom builtin).
        // Kaldırma güvenli: bytecode diske serileştirilmiyor (eski .lkc cache kaldırıldı,
        // bkz. cgi_main.cpp), index'ler process-içi geçici; VM her başlangıçta yeniden derler.
        // bare join — interpreter'da global (fn_name=="join", use gerektirmez); VM'de
        // yoktu → route içinde CALL'a düşüp fallback ediyordu. Global builtin olarak
        // bağlanır (string::join'e alias DEĞİL — o `use string` gerektirir → divergence).
        "join",
        // math:: tamamlama — interpreter math modülünde vardı ama builtin_names'de
        // YOKTU → route içinde CALL'a düşüp KALICI interpreter'a iniyordu (array:: ile
        // aynı fallback landmine; math-yoğun route sessizce yavaş yola/koda düşüyordu).
        // Sona eklendi (mevcut indexler kaymaz); auto-wire math modülünden bağlar.
        "math::round", "math::pow", "math::sqrt", "math::min", "math::max",
        "math::sin", "math::cos", "math::tan", "math::log", "math::pi", "math::random",
        // string:: tamamlama — format + regex (aynı landmine; regex/format'lı route'lar
        // fallback ediyordu). Auto-wire string modülünden bağlar.
        "string::format", "string::regex_match", "string::regex_replace", "string::regex_match_all",
        "string::decode",  // charset → UTF-8 (iso-8859-9 etc.)
        // type:: modülü — builtin_names'de HİÇ yoktu (bare is_int/is_string ayrı, index
        // 10-15). `use type` ile type::is_int/of/to_int çağıran route fallback ediyordu.
        "type::is_null", "type::is_int", "type::is_float", "type::is_string", "type::is_bool",
        "type::is_array", "type::is_function", "type::of",
        "type::to_int", "type::to_float", "type::to_string", "type::to_bool",
        // crypto:: tamamlama — base64/hex/hmac_sha256/constant_compare/random_string/
        // uuid/rs256 (JWT imzalama). Hepsi interpreter crypto modülünde DOĞRULANDI.
        "crypto::base64_encode", "crypto::base64_decode",
        "crypto::base64url_encode", "crypto::base64url_decode",
        "crypto::hex_encode", "crypto::hex_decode",
        "crypto::hmac_sha256", "crypto::hmac_sha256_raw", "crypto::constant_compare",
        "crypto::random_string", "crypto::uuid",
        "crypto::rs256_sign", "crypto::rs256_sign_b64url", "crypto::rs256_verify",
        // request:: tamamlama — is_get/is_post/... method kısayolları + file (upload).
        // Yoktular → bunları kullanan her route KALICI interpreter'a düşüyordu.
        "request::is_get", "request::is_post", "request::is_put", "request::is_delete",
        "request::is_head", "request::is_options", "request::is_patch", "request::file",
        // db:: tamamlama — transaction/begin/commit/rollback/escape. transaction()
        // closure alır: VM'den BYTECODE_FN gelir, interp->invoke() köprüyle çalıştırır.
        "db::transaction", "db::begin", "db::commit", "db::rollback", "db::escape",
        // exit()/die() — VM'de YOKTU: CLI-VM'de "Not callable" fırlatıyordu (CLI
        // script'lerinde exit kodu yaygın), web'de route'u kalıcı interpreter'a düşürüyordu.
        // Her iki motorda ExitException'a bağlanır (interpreter ile aynı semantik).
        "exit", "die",
        // ── 256+ bölge: CALL_BUILTIN indeksi artık 16-bit (NOP hint'in b alanı yüksek
        // 8 biti taşır) — 8-bit duvar kalktı. Aşağıdakiler interpreter modüllerinde vardı
        // ama builtin_names'de yoktu → kullanan route KALICI interpreter'a düşüyordu
        // (çıktı doğru ama ~40× yavaş; CLI-VM'de execution-öncesi tree-walk'a düşüyordu).
        // Denetlendi: callback alan yalnız http::stream (BYTECODE_FN'i zaten tanır).
        "cache::keys", "cache::size",
        "queue::clear", "queue::names", "queue::peek",
        "template::escape", "template::render_string",
        "validator::check",
        "log::configure", "log::memory",
        "http::post_json", "http::stream",
        "mail::provider", "mail::send_html",
        "file::upload_dir",
        "route::matched", "route::param",
        "runtime::gc", "runtime::stats",
        "look::check",
        // jobs:: — worker HARİÇ: jobs::worker($q,$fn) setup'ta handler kaydeder, job'lar
        // istek/VM bağlamı DIŞINDA çalışır → VM closure kaydedilirse "no active VM".
        "jobs::push", "jobs::next", "jobs::done", "jobs::fail", "jobs::failed",
        "jobs::list", "jobs::stats", "jobs::retry", "jobs::purge", "jobs::recover",
        // session::has — docs'ta belgeliydi ama eksikti (dogfooding #1). Auto-wire
        // name→index dinamik, konum önemsiz; bytecode diske yazılmadığından (bkz. yukarı)
        // araya da eklenebilirdi — sona eklemek yalnızca diff temizliği için.
        "session::has",
        // route::group — setup-zamanı prefix+middleware kalıtımı (Go/chi tarzı). Closure alır,
        // içindeki route() çağrıları grup prefix'ini/mw'lerini miras alır. Bkz. route_group.h.
        "route::group",
        // args — CLI argümanları (bare builtin). `lk x.lk a b` → args() == ["a","b"].
        // Web/CGI'de boş dizi. Sona eklendi (indeks kayması yok).
        "args",
    };
    return NAMES;
}

// CLI argümanları — süreç-global, main() doldurur. Varsayılan boş (web/CGI).
std::vector<std::string>& script_args() {
    static std::vector<std::string> ARGS;
    return ARGS;
}

int builtin_index(const std::string& name) {
    static const std::unordered_map<std::string, int> INDEX = [] {
        std::unordered_map<std::string, int> m;
        const auto& names = builtin_names();
        for (int i = 0; i < (int)names.size(); ++i) m[names[i]] = i;
        return m;
    }();
    auto it = INDEX.find(name);
    return it == INDEX.end() ? -1 : it->second;
}

bool is_reserved_builtin(const std::string& name) {
    // Rezerve = builtin_names()'in bare (":: içermeyen") girdileri. Tek kaynak,
    // el-listesi yok → yeni bare builtin eklenince otomatik dahil olur.
    static const std::unordered_set<std::string> RESERVED = [] {
        std::unordered_set<std::string> s;
        for (const auto& n : builtin_names())
            if (n.find("::") == std::string::npos) s.insert(n);
        return s;
    }();
    return RESERVED.count(name) != 0;
}

} // namespace look

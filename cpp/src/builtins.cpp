// builtins.cpp — VM builtin tablosunun tek kaynağı (bkz. builtins.h)

#include "look/builtins.h"

#include <unordered_map>

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
        "http::patch", "http::head", "http::request",
        // 120-123: template::
        "template::render", "template::include", "template::block", "template::extends",
        // 124-128: cache::
        "cache::get", "cache::set", "cache::has", "cache::delete", "cache::flush",
        // 129-132: queue::
        "queue::push", "queue::pop", "queue::size", "queue::flush",
        // 133-134: mail::
        "mail::send", "mail::deliver_maildir",
        // 135-140: crypto::
        "crypto::sha256", "crypto::sha512", "crypto::md5",
        "crypto::hmac", "crypto::random_bytes", "crypto::random_hex",
        // 141-142: base64::
        "base64::encode", "base64::decode",
        // 143: uuid::
        "uuid::v4",
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
    };
    return NAMES;
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

} // namespace look

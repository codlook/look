// builtins.cpp — the single source of the VM builtin table (see builtins.h)

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
        // 16-19: channel (special opcodes — CHAN_SEND/RECV — but listed for the fallback)
        "send", "receive", "close", "chan_size",
        // 20-21: json (old naming — backward compatibility)
        "json_encode", "json_decode",
        // 22: route — registers a closure at setup, no-op at dispatch
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
        // 56: db::connect (runs at setup — creates the pool, returns a key string)
        "db::connect",
        // 57: channel() — creates a LookChannel
        "channel",
        // 58: env() — also called at setup (building the DB DSN)
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
        "file::remove", "file::size", "file::store", "file::list",
        // 113-119: http:: client
        "http::get", "http::post", "http::put", "http::delete",
        "http::patch",   // http::head/request PHANTOMS REMOVED (no impl; get/post/... covers them)
        // 120-123: template::
        "template::render", "template::include", "template::block", "template::extends",
        // 124-128: cache::
        "cache::get", "cache::set", "cache::has", "cache::delete", "cache::flush",
        // 129-132: queue::
        "queue::push", "queue::pop", "queue::size",   // queue::flush PHANTOM REMOVED (the real one is queue::clear)
        // 133-134: mail::
        "mail::send", "mail::deliver_maildir",
        // crypto:: — PHANTOMS REMOVED (docs-conformance/measurement): sha512/md5/hmac/random_hex
        // were in the builtin list but have NO implementation in the crypto module ("no function" when called).
        // Real counterparts: sha256 exists; hmac→hmac_sha256; random_hex→random_bytes+hex_encode.
        // base64::encode/decode were PHANTOMS too (there is NO base64 module → it was confused with the json module);
        // the real base64 is crypto::base64_encode/decode. uuid::v4 PHANTOM → the real one is crypto::uuid.
        "crypto::sha256", "crypto::random_bytes",
        // 144-147: parallel:: observability
        "parallel::active", "parallel::wait", "parallel::limit", "parallel::at_capacity",
        // 148-151: error::
        "error::new", "error::is", "error::message", "error::code",
        // 152: response::json
        "response::json",
        // 153: before_route() — global middleware registration
        "before_route",
        // 154: stop() — cancel route execution from middleware
        "stop",
        // 155-161: scalar core builtins
        "strlen", "abs", "max", "min", "sqrt", "strtoupper", "strtolower",
        // 162-163: request:: reading incoming headers (from headers_in)
        "request::header", "request::headers",
        // 164: response::error — status code + error JSON (single line)
        "response::error",
        // 165-168: app:: — service registry (app::set/get/has/db)
        "app::set", "app::get", "app::has", "app::db",
        // 169-170: response:: body helpers (VM-native dispatch)
        "response::text", "response::html",
        // 171+: array:: module — it used to be ABSENT from builtin_names → when a route called
        // array::map/keys/sort etc. the compiler could not emit CALL_BUILTIN and fell back to the generic
        // CALL, threw "Not callable" at runtime and the route PERMANENTLY dropped to the
        // interpreter (data-heavy routes ended up entirely on the slow path).
        // Appended at the end (existing indexes do not shift); auto-wired with `use array`.
        "array::map", "array::filter", "array::reduce", "array::find", "array::keys",
        "array::values", "array::sort", "array::reverse", "array::slice", "array::contains",
        "array::unique", "array::flatten", "array::chunk", "array::zip", "array::all",
        "array::any", "array::push", "array::pop", "array::set", "array::new_assoc",
        // NOTE: array::random_bytes/random_string/hex_encode/hex_decode/constant_compare/uuid
        // REMOVED (found by the docs-conformance scan) — they were wrong copies of the crypto:: functions
        // in the array:: namespace; the array module has NO counterparts →
        // calling them gave "'array' has no function" / "Module not loaded" (phantom builtin).
        // Removal is safe: bytecode is not serialized to disk (the old .lkc cache was removed,
        // see cgi_main.cpp), indexes are transient within the process; the VM recompiles on every start.
        // bare join — global (needs no use; it was inline in the old interpreter); it was missing
        // in the VM → inside a route it fell to CALL and used the fallback. It is bound as a global
        // builtin (NOT an alias of string::join — that needs `use string` → divergence).
        "join",
        // math:: completion — it existed in the math module but was MISSING from builtin_names
        // → inside a route it fell to CALL and PERMANENTLY dropped to the interpreter (same fallback
        // landmine as array::; math-heavy routes silently ended up on the slow path).
        // Appended at the end (existing indexes do not shift); auto-wire binds from the math module.
        "math::round", "math::pow", "math::sqrt", "math::min", "math::max",
        "math::sin", "math::cos", "math::tan", "math::log", "math::pi", "math::random",
        // string:: completion — format + regex (same landmine; routes using regex/format
        // used the fallback). Auto-wire binds from the string module.
        "string::format", "string::regex_match", "string::regex_replace", "string::regex_match_all",
        "string::decode",  // charset → UTF-8 (iso-8859-9 etc.)
        // type:: module — it was ABSENT from builtin_names altogether (bare is_int/is_string are separate, index
        // 10-15). A route calling type::is_int/of/to_int via `use type` used the fallback.
        "type::is_null", "type::is_int", "type::is_float", "type::is_string", "type::is_bool",
        "type::is_array", "type::is_function", "type::of",
        "type::to_int", "type::to_float", "type::to_string", "type::to_bool",
        // crypto:: completion — base64/hex/hmac_sha256/constant_compare/random_string/
        // uuid/rs256 (JWT signing). All VERIFIED in the crypto module.
        "crypto::base64_encode", "crypto::base64_decode",
        "crypto::base64url_encode", "crypto::base64url_decode",
        "crypto::hex_encode", "crypto::hex_decode",
        "crypto::hmac_sha256", "crypto::hmac_sha256_raw", "crypto::constant_compare",
        "crypto::random_string", "crypto::uuid",
        "crypto::rs256_sign", "crypto::rs256_sign_b64url", "crypto::rs256_verify",
        // request:: completion — is_get/is_post/... method shortcuts + file (upload).
        // They were missing → every route using them PERMANENTLY dropped to the interpreter.
        "request::is_get", "request::is_post", "request::is_put", "request::is_delete",
        "request::is_head", "request::is_options", "request::is_patch", "request::file",
        // db:: completion — transaction/begin/commit/rollback/escape. transaction()
        // takes a closure: BYTECODE_FN arrives from the VM, interp->invoke() runs it through the bridge.
        "db::transaction", "db::begin", "db::commit", "db::rollback", "db::escape",
        // exit()/die() — were MISSING in the VM: in CLI-VM they threw "Not callable" (exit codes
        // are common in CLI scripts), on the web they dropped the route to the interpreter permanently.
        // Bound to ExitException.
        "exit", "die",
        // ── 256+ region: the CALL_BUILTIN index is now 16-bit (the b field of the NOP hint carries the high
        // 8 bits) — the 8-bit wall is gone. The ones below existed in the modules
        // but not in builtin_names → a route using them PERMANENTLY dropped to the interpreter
        // (output correct but ~40× slower; in CLI-VM it fell to tree-walk before execution).
        // Audited: the only one taking a callback is http::stream (it already recognizes BYTECODE_FN).
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
        // jobs:: — EXCEPT worker: jobs::worker($q,$fn) registers a handler at setup, jobs run
        // OUTSIDE the request/VM context → if a VM closure were registered, "no active VM".
        "jobs::push", "jobs::next", "jobs::done", "jobs::fail", "jobs::failed",
        "jobs::list", "jobs::stats", "jobs::retry", "jobs::purge", "jobs::recover",
        // session::has — it was documented in the docs but missing (dogfooding #1). Auto-wire
        // name→index is dynamic, position does not matter; since bytecode is not written to disk (see above)
        // it could have been inserted in the middle — appending is only for a clean diff.
        "session::has",
        // route::group — setup-time prefix+middleware inheritance (Go/chi style). Takes a closure,
        // route() calls inside it inherit the group's prefix/middlewares. See route_group.h.
        "route::group",
        // args — CLI arguments (bare builtin). `lk x.lk a b` → args() == ["a","b"].
        // Empty array on Web/CGI. Appended at the end (no index shift).
        "args",
        // db::driver — the connected driver ("sqlite"/"mysql"/"postgres"); for dialect-aware
        // code (migrate schema→DDL). mod::fn → http_main wires it automatically.
        "db::driver",
        // date::mono_us — monotonic microsecond clock (benchmark timing). Appended at
        // end (no index shift). Without it the compiler treats it as an unknown module
        // function (before LOOK 2 that sent the whole script to tree-walk).
        "date::mono_us",
        // timer:: — a timer set up in a route runs in the VM (the callback runs, like a new
        // request, in its own VM and with clean setup values — http_main.cpp). It used to exist
        // only inside the interpreter; a route using it had to be handed to the interpreter.
        "timer::after",
        "timer::every",
        "timer::cancel",
        // jobs::worker / jobs::run — queue handler registration and the runner exist in the VM too.
        // (jobs::worker used not to be a builtin → every web app calling it at top level
        // fell entirely to the interpreter.)
        "jobs::worker",
        "jobs::run",
        // ws:: / sse:: — functions that work on a connection handle and take no callback.
        // Because of these an ordinary route (e.g. ws::broadcast inside POST /notify) used to be handed
        // to the interpreter. ws::on / sse::on take a callback — separately below.
        "ws::send", "ws::close", "ws::broadcast", "ws::clients",
        "sse::send", "sse::close", "sse::clients",
        // ws::on / sse::on — connection handlers are in the VM too: the callback is set up like a new request.
        "ws::on", "sse::on",
        // timer::wait([ms]) — waits for one-shot timers on the command line (same form as parallel::wait).
        // The process ends when the script ends; waiting is written explicitly.
        "timer::wait",
        // request::scheme/host — the scheme and host as the client saw them (with the trusted-proxy rule).
        "request::scheme", "request::host",
        // csrf:: — stateless CSRF protection on the server (signed double-submit cookie, LOOK_SECRET). Check is
        // default: in an app using cookies, every request other than GET/HEAD/OPTIONS (the http_main gate); exempt is an exemption.
        "csrf::token", "csrf::field", "csrf::exempt",
        // type::is_map — list or dictionary (type::of says "array" for both). List-specific
        // functions error on a dictionary, so incoming data must be checkable. Appended at the end.
        "type::is_map",
        // ws::send_binary / ws::broadcast_binary — binary frame (opcode 0x2); ws::send requires valid UTF-8.
        // Appended at the end.
        "ws::send_binary", "ws::broadcast_binary",
    };
    return NAMES;
}

// CLI arguments — process-global, filled by main(). Empty by default (web/CGI).
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

// ── Builtin effects (builtins.h: BuiltinEffect) ─────────────────────────────────────────────
// WHEN ADDING A NEW BUILTIN add it here too — a name with no class turns CI red (builtin_effect_table_test).
// The class is given by MEASUREMENT: e.g. global push/pop change the array in place (Effect), array::push
// returns a new array (Pure) — by trying, not by guessing.
namespace {
struct EffectRow { BuiltinEffect e; std::vector<const char*> names; };
const std::vector<EffectRow>& effect_rows() {
    static const std::vector<EffectRow> ROWS = {
        { BuiltinEffect::Pure, {
            "count", "str", "int", "float", "bool", "string",
            "is_null", "is_int", "is_float", "is_string", "is_bool", "is_array",
            "json_encode", "strlen", "abs", "max", "min", "sqrt", "strtoupper", "strtolower", "join",
            "array::keys", "array::values", "array::reverse", "array::slice", "array::contains",
            "array::unique", "array::flatten", "array::chunk", "array::zip", "array::push", "array::pop",
            "array::set", "array::new_assoc",
            "auth::verify",
            "crypto::sha256", "crypto::base64_encode", "crypto::base64url_encode", "crypto::hex_encode",
            "crypto::hmac_sha256", "crypto::hmac_sha256_raw", "crypto::constant_compare",
            "date::format", "date::add", "date::sub", "date::diff", "date::from_timestamp",
            "date::is_valid", "date::weekday", "date::week",
            "error::is", "error::message", "error::code",
            "html::escape", "html::attr", "html::strip",
            "json::encode",
            "math::abs", "math::floor", "math::ceil", "math::round", "math::pow", "math::sqrt",
            "math::min", "math::max", "math::sin", "math::cos", "math::tan", "math::log", "math::pi",
            "string::upper", "string::lower", "string::trim", "string::replace", "string::contains",
            "string::substr", "string::split", "string::join", "string::len", "string::starts_with",
            "string::ends_with", "string::reverse", "string::repeat", "string::index_of",
            "string::pad_left", "string::pad_right", "string::slugify", "string::ltrim", "string::rtrim",
            "string::format",
            "template::escape",
            "type::is_null", "type::is_int", "type::is_float", "type::is_string", "type::is_bool",
            "type::is_array", "type::is_function", "type::of", "type::to_int", "type::to_float",
            "type::to_string", "type::to_bool", "type::is_map",
            "validator::required", "validator::email", "validator::integer", "validator::numeric",
            "validator::min", "validator::max", "validator::check",
        }},
        { BuiltinEffect::Reads, {
            "chan_size", "env", "config", "args",
            "app::get", "app::has", "app::db",
            "auth::hash",   // random salt
            "cache::get", "cache::has", "cache::keys", "cache::size",
            "cookie::get", "cookie::has",
            "crypto::random_bytes", "crypto::random_string", "crypto::uuid",
            "date::now", "date::today", "date::timestamp", "date::mono_us",
            "db::escape", "db::driver", "db::last_id", "db::affected",
            "file::read", "file::exists", "file::size", "file::list", "file::upload_dir",
            "jobs::failed", "jobs::list", "jobs::stats",
            "log::memory", "look::check", "mail::provider", "math::random",
            "parallel::active", "parallel::at_capacity",
            "queue::size", "queue::names", "queue::peek",
            "request::method", "request::get", "request::post", "request::json", "request::body",
            "request::path", "request::ip", "request::param", "request::all", "request::header",
            "request::headers", "request::is_get", "request::is_post", "request::is_put",
            "request::is_delete", "request::is_head", "request::is_options", "request::is_patch",
            "request::file", "request::scheme", "request::host",
            "route::matched", "route::param", "runtime::stats",
            "session::get", "session::has", "string::random",
            "sse::clients", "ws::clients",
        }},
        { BuiltinEffect::Effect, {
            "print", "write", "push", "pop", "send", "receive", "close", "route", "channel",
            "before_route", "stop", "exit", "die",
            // error::new ALWAYS throws (no throw needed — documented usage); it was taken for Pure and, alone
            // on a line, produced a "result unused" compile error.
            "error::new",
            "app::set",
            "cache::set", "cache::delete", "cache::flush",
            "cookie::set", "cookie::delete",
            "csrf::token", "csrf::field",   // sets/renews the cookie
            "csrf::exempt",                 // exemption registration at setup
            "crypto::rs256_sign", "crypto::rs256_sign_b64url",   // loads a key, throws on a bad key
            "db::query", "db::exec", "db::col", "db::close", "db::connect", "db::transaction",
            "db::begin", "db::commit", "db::rollback",
            "file::put", "file::append", "file::remove", "file::store",
            "http::get", "http::post", "http::put", "http::delete", "http::patch", "http::post_json",
            "http::stream",
            "jobs::push", "jobs::next", "jobs::done", "jobs::fail", "jobs::retry", "jobs::purge",
            "jobs::recover", "jobs::worker", "jobs::run",
            "log::info", "log::warn", "log::error", "log::debug", "log::query", "log::configure",
            "mail::send", "mail::deliver_maildir", "mail::send_html",
            "parallel::wait", "parallel::limit",
            "queue::push", "queue::pop", "queue::clear",
            "response::status", "response::header", "response::redirect", "response::json",
            "response::error", "response::text", "response::html",
            "route::group", "runtime::gc",
            "session::start", "session::set", "session::destroy", "session::regenerate",
            "sse::send", "sse::close", "sse::on",
            "template::render", "template::include", "template::block", "template::extends",
            "template::render_string",
            "timer::after", "timer::every", "timer::cancel", "timer::wait",
            "ws::send", "ws::close", "ws::broadcast", "ws::on", "ws::send_binary", "ws::broadcast_binary",
        }},
        { BuiltinEffect::Callback, {
            "array::map", "array::filter", "array::reduce", "array::find", "array::sort",
            "array::all", "array::any",
        }},
        { BuiltinEffect::ThrowCheck, {
            "json_decode", "json::decode", "date::parse",
            "crypto::base64_decode", "crypto::base64url_decode", "crypto::hex_decode",
            "crypto::rs256_verify",
            "string::regex_match", "string::regex_replace", "string::regex_match_all", "string::decode",
        }},
    };
    return ROWS;
}
const std::unordered_map<std::string, BuiltinEffect>& effect_map() {
    static const std::unordered_map<std::string, BuiltinEffect> M = [] {
        std::unordered_map<std::string, BuiltinEffect> m;
        for (const auto& row : effect_rows())
            for (const char* n : row.names) m.emplace(n, row.e);
        return m;
    }();
    return M;
}
} // namespace

BuiltinEffect builtin_effect(const std::string& name) {
    auto it = effect_map().find(name);
    return it == effect_map().end() ? BuiltinEffect::Unknown : it->second;
}

std::vector<std::string> builtin_effect_strays() {
    std::vector<std::string> out;
    for (const auto& row : effect_rows())
        for (const char* n : row.names)
            if (builtin_index(n) < 0) out.push_back(n);
    return out;
}

bool is_reserved_builtin(const std::string& name) {
    // Reserved = the bare (no "::") entries of builtin_names(). Single source,
    // no hand list → included automatically when a new bare builtin is added.
    static const std::unordered_set<std::string> RESERVED = [] {
        std::unordered_set<std::string> s;
        for (const auto& n : builtin_names())
            if (n.find("::") == std::string::npos) s.insert(n);
        return s;
    }();
    return RESERVED.count(name) != 0;
}

} // namespace look

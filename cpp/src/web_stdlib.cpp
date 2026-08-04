#define _CRT_RAND_S
#include "look/stdlib.h"
#include "look/web.h"
#include "look/fiber.h"
#include <cmath>
#include <cstdint>
#include "look/interpreter.h"
#include "look/mysql_client.h"
#include "look/sqlite_client.h"
#include "look/postgres_client.h"
#include "look/resp_client.h"
#include "look/logger.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <regex>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <filesystem>
#include <system_error>
#include "look/json_str_parse.h"   // saf JSON string-unescape (fuzz ile paylasilan tek tanim)
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace look {

// â”€â”€ JSON encode/decode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static std::string json_encode(const Value& v, int depth = 0);

static std::string json_encode_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}

static std::string json_encode(const Value& v, int depth) {
    // Cycle/derinlik guard — LOOK dizileri referans tipi (shared_ptr); `$a[0]=$a`
    // ile kendine-referanslı yapı kurulabilir → decode'da guard vardı ama encode'da
    // YOKTU → sonsuz özyineleme → SIGSEGV/worker crash. 256 derinlikte kes.
    if (depth > 256) return "null";
    switch (v.type()) {
        case Value::INT:    return std::to_string(v.as_int());
        case Value::FLOAT:  {
            double d = v.as_float();
            // NaN/Infinity JSON'da geçersiz — null olarak serileştir.
            if (!std::isfinite(d)) return "null";
            return look_format_double(d);
        }
        case Value::STRING: return json_encode_string(v.as_string());
        case Value::BOOL:   return v.as_bool() ? "true" : "false";
        case Value::NONE:   return "null";
        case Value::ARRAY: {
            auto& arr = *v.as_array();
            // Associative array / struct: ["__assoc__", k0, v0, k1, v1, ...]
            if (!arr.empty() && arr[0].type() == Value::STRING &&
                arr[0].as_string() == "__assoc__") {
                std::string out = "{";
                bool first = true;
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    // Skip internal runtime tags (__struct__ etc.)
                    if (arr[i].to_string() == "__struct__") continue;
                    if (!first) out += ",";
                    first = false;
                    out += json_encode_string(arr[i].to_string());
                    out += ":";
                    out += json_encode(arr[i + 1], depth + 1);
                }
                return out + "}";
            }
            // Regular array
            std::string out = "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += json_encode(arr[i], depth + 1);
            }
            return out + "]";
        }
        default: return "null";
    }
}

// Simple JSON decode â€” returns Value
// Derinlik limiti — derin iç içe JSON ([[[[...]]]] veya {...}) C++ stack'ini
// taşırıp SIGSEGV/worker crash'e yol açar. request::json() HTTP body'yi decode
// ettiğinden bu network-facing DoS'tur. 256 gerçek JSON için fazlasıyla yeterli.
static constexpr int JSON_MAX_DEPTH = 256;
static Value json_decode_value(const std::string& s, size_t& i, int depth = 0);

static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
}


// Derinlik aşımında TÜM parse iptal edilir (yalnız o dalı null'lamak yetmez:
// pozisyon i ilerlemediği için dış döngü sonsuza dek null push edip bad_alloc/
// hang yapardı). Public giriş noktalarında yakalanır → temiz null döner.
struct JsonTooDeep {};
// Bozuk JSON (ilerleme sağlamayan karakter) → parse'ı iptal et. Aksi halde array/
// object döngüsünde `i` ilerlemez ve SONSUZ DÖNGÜ olur (auth gerektirmeyen uzaktan
// DoS: tek `[a]` isteği worker'ı %100 CPU'da kilitler). Public giriş noktalarında
// yakalanır → temiz null döner.
struct JsonParseError {};

static Value json_decode_value(const std::string& s, size_t& i, int depth) {
    if (depth > JSON_MAX_DEPTH) throw JsonTooDeep{};  // stack + heap DoS savunması
    json_skip_ws(s, i);
    if (i >= s.size()) return Value();

    if (s[i] == '"') {
        return Value(json_decode_str(s, i));
    }
    // JSON object { } â†’ assoc array
    if (s[i] == '{') {
        i++;
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        json_skip_ws(s, i);
        while (i < s.size() && s[i] != '}') {
            size_t before = i;                       // ilerleme güvencesi
            // key — açılış tırnağı yoksa bozuk (aksi halde ilerlemeyebilir)
            if (s[i] != '"') throw JsonParseError{};
            std::string key = json_decode_str(s, i);
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ':') i++;
            json_skip_ws(s, i);
            // value
            Value val = json_decode_value(s, i, depth + 1);
            // B-04: yinelenen anahtar → SON kazanır, anahtar KONUMU korunur
            // (JS/Go/Python/PHP ile aynı). Eski davranış "ilk kazanır + ikisini de tut"
            // idi → gateway "user" görüp geçirir, backend "admin" görür (JSON interop
            // yetki atlatma). Yerinde güncelle: anahtar sona kaymasın (o da uyumsuzluk).
            bool dup = false;
            for (size_t k = 1; k + 1 < arr->size(); k += 2) {
                if ((*arr)[k].type() == Value::STRING && (*arr)[k].to_string() == key) {
                    (*arr)[k + 1] = val;   // konum sabit, değer güncellenir
                    dup = true;
                    break;
                }
            }
            if (!dup) { arr->push_back(Value(key)); arr->push_back(val); }
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ',') i++;
            json_skip_ws(s, i);
            if (i == before) throw JsonParseError{};  // ilerleme yok → sonsuz döngü engeli
        }
        if (i < s.size()) i++; // skip }
        return Value(arr);
    }

    if (s[i] == '[') {
        i++;
        auto arr = std::make_shared<std::vector<Value>>();
        json_skip_ws(s, i);
        while (i < s.size() && s[i] != ']') {
            size_t before = i;                       // ilerleme güvencesi
            arr->push_back(json_decode_value(s, i, depth + 1));
            json_skip_ws(s, i);
            if (i < s.size() && s[i] == ',') i++;
            json_skip_ws(s, i);
            if (i == before) throw JsonParseError{};  // ilerleme yok → sonsuz döngü engeli
        }
        if (i < s.size()) i++; // skip ]
        return Value(arr);
    }
    if (s.substr(i, 4) == "true")  { i += 4; return Value(true); }
    if (s.substr(i, 5) == "false") { i += 5; return Value(false); }
    if (s.substr(i, 4) == "null")  { i += 4; return Value(); }
    // Number — geçerli bir sayı başlangıcı DEĞİLSE reddet. Aksi halde `i`
    // ilerlemeden Value(0) dönerdi → çağıran döngüde sonsuz döngü (DoS). Ayrıca
    // json::decode("garbage")→0 correctness bug'ını da giderir.
    if (s[i] != '-' && !std::isdigit((unsigned char)s[i]))
        throw JsonParseError{};
    // Number
    size_t start = i;
    bool is_float = false;
    if (s[i] == '-') i++;
    while (i < s.size() && (std::isdigit(s[i]) || s[i]=='.' || s[i]=='e' || s[i]=='E' || s[i]=='+' || s[i]=='-')) {
        if (s[i]=='.' || s[i]=='e' || s[i]=='E') is_float = true;
        i++;
    }
    std::string num = s.substr(start, i - start);
    if (num.empty() || num == "-") return Value((int64_t)0);
    try {
        if (is_float) return Value(std::stod(num));
        return Value((int64_t)std::stoll(num));   // 32-bit stoi taşıp 0 döndürüyordu
    } catch (...) {
        // int64 aralığını AŞAN tamsayı: float'a düşmek SESSİZ kesinlik kaybıdır.
        // 9223372036854775809 → 9223372036854775808 (1 eksik), uint64-max
        // 18446744073709551615 → ...616 (1 fazla). Snowflake/Discord/Twitter ID,
        // MySQL BIGINT UNSIGNED anahtar bu aralıkta — ID sessizce bozulur, yanlış
        // kayıt eşleşir. `...808` makul bir tamsayı gibi göründüğü için `1e+27`den
        // çok daha tehlikeli. KESİNLİĞİ KORU: orijinal metni STRING olarak döndür
        // (LOOK'ta uint64/bignum tipi yok; string tam kimliği kayıpsız taşır ve
        // DB'ye/karşılaştırmaya birebir gider). Float form (is_float) zaten
        // kesinlik kaybını göze aldığı için double kalır.
        if (!is_float) return Value(num);
        try { return Value(std::stod(num)); } catch (...) { return Value((int64_t)0); }
    }
}

// â”€â”€ Route matching â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Convert route pattern "/users/{id}" to regex "^/users/([^/]+)$"
// Returns param names in order

static std::regex pattern_to_regex(const std::string& pattern,
                                    std::vector<std::string>& param_names) {
    std::string reg = "^";
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == '{') {
            size_t end = pattern.find('}', i);
            if (end == std::string::npos) break;
            param_names.push_back(pattern.substr(i + 1, end - i - 1));
            reg += "([^/]+)";
            i = end + 1;
        } else if (pattern[i] == '/') {
            reg += "/";
            i++;
        } else {
            // Escape regex special chars
            static const std::string special = ".^$*+?()[]{}|\\";
            if (special.find(pattern[i]) != std::string::npos)
                reg += "\\";
            reg += pattern[i++];
        }
    }
    reg += "$";
    return std::regex(reg);
}

// â”€â”€ Web modules â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static Module make_request(WebContext* ctx) {
    Module m;
    m.name = "request";

    m.functions["method"] = [ctx](auto) -> Value {
        return Value(ctx->method);
    };
    m.functions["path"] = [ctx](auto) -> Value {
        return Value(ctx->path);
    };
    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) {
            // Return all as array
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& [k, v] : ctx->get_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->get_params.find(args[0].to_string());
        // Ikinci argüman VARSAYILAN deger — env(key,varsayilan) / config(key,varsayilan)
        // ile ayni kalip. ESKI HATA: ikinci argüman SESSIZCE YOK SAYILIYORDU;
        // request::get("sayfa", 1) yazan gelistirici null aliyordu.
        if (it != ctx->get_params.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();
    };
    m.functions["post"] = [ctx](auto args) -> Value {
        if (args.empty()) {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& [k, v] : ctx->post_params) arr->push_back(Value(v));
            return Value(arr);
        }
        auto it = ctx->post_params.find(args[0].to_string());
        if (it != ctx->post_params.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();   // varsayilan (env kalibi)
    };
    m.functions["body"] = [ctx](auto) -> Value {
        return Value(ctx->body);
    };
    m.functions["ip"] = [ctx](auto) -> Value {
        return Value(ctx->remote_addr);
    };
    // request::header("Authorization") — gelen HTTP header'ını oku.
    // Anahtar case-insensitive (headers_in lowercase saklanır: Authorization,
    // X-Api-Key, User-Agent hepsi çalışır). Yoksa null döner.
    m.functions["header"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        std::string key = args[0].to_string();
        for (auto& c : key) c = (char)std::tolower((unsigned char)c);
        auto it = ctx->headers_in.find(key);
        if (it != ctx->headers_in.end()) return Value(it->second);
        return args.size() >= 2 ? args[1] : Value();   // varsayilan (env kalibi)
    };
    // request::headers() — tüm header'ları assoc array olarak döner
    m.functions["headers"] = [ctx](auto) -> Value {
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        for (auto& [k, v] : ctx->headers_in) { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        return Value(arr);
    };
    m.functions["is_get"]     = [ctx](auto) -> Value { return Value(ctx->method == "GET");     };
    m.functions["is_post"]    = [ctx](auto) -> Value { return Value(ctx->method == "POST");    };
    m.functions["is_put"]     = [ctx](auto) -> Value { return Value(ctx->method == "PUT");     };
    m.functions["is_delete"]  = [ctx](auto) -> Value { return Value(ctx->method == "DELETE");  };
    m.functions["is_patch"]   = [ctx](auto) -> Value { return Value(ctx->method == "PATCH");   };
    m.functions["is_head"]    = [ctx](auto) -> Value { return Value(ctx->method == "HEAD");    };
    m.functions["is_options"] = [ctx](auto) -> Value { return Value(ctx->method == "OPTIONS"); };
    // request::json() â€” parse JSON body otomatik
    m.functions["json"] = [ctx](auto) -> Value {
        if (ctx->body.empty()) return Value();
        size_t i = 0;
        try { return json_decode_value(ctx->body, i); }
        catch (const JsonTooDeep&) { return Value(); }   // aşırı derin gövde → null (DoS savunması)
        catch (const JsonParseError&) { return Value(); } // bozuk gövde → null (sonsuz döngü DoS savunması)
    };
    // request::all() â€” GET + POST params birleÅŸik
    m.functions["all"] = [ctx](auto) -> Value {
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("__assoc__")));
        for (auto& [k, v] : ctx->get_params)  { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        for (auto& [k, v] : ctx->post_params) { arr->push_back(Value(k)); arr->push_back(Value(v)); }
        return Value(arr);
    };
    m.functions["param"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->route_params.find(args[0].to_string());
        return it != ctx->route_params.end() ? Value(it->second) : Value();
    };

    // request::file(name) veya request::file(name, options_array)
    // options keys: max_size (int bytes), allow_mime (array), allow_svg (bool)
    // Varsayılan limit: 1MB
    m.functions["file"] = [ctx](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("request::file() requires field name");

        // multipart/form-data DEĞİLSE dosya YOK demektir → docs sözleşmesi: null döner
        // (kullanıcı `if ($f == null)` ile ele alır). ESKİ HATA: throw ediyordu → docs
        // "null döner" derken kod exception atıyordu; dokümanı takip eden `if($f==null)`
        // dalına HİÇ ulaşamıyordu (dogfooding #3-A, session::has'ın kardeşi). Ayrıca ileride
        // multipart --mode http'ye tam bağlanınca da doğru sözleşme: dosya-yoksa-null.
        if (ctx->content_type.find("multipart/form-data") == std::string::npos)
            return Value();  // null — dosya yok

        std::string field = args[0].to_string();
        auto it = ctx->uploaded_files.find(field);
        if (it == ctx->uploaded_files.end() || !it->second.valid)
            return Value(); // dosya seçilmemişse null döndür

        const UploadedFile& uf = it->second;

        // Seçenekleri oku
        size_t    max_size   = 1 * 1024 * 1024; // varsayılan 1MB
        bool      allow_svg  = false;
        std::vector<std::string> allow_mime;

        if (args.size() >= 2 && args[1].type() == Value::ARRAY) {
            auto& arr = *args[1].as_array();
            bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING
                            && arr[0].to_string() == "__assoc__";
            if (is_assoc) {
                for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                    std::string key = arr[i].to_string();
                    Value&      val = arr[i+1];
                    if (key == "max_size")  max_size  = (size_t)val.to_int();
                    if (key == "allow_svg") allow_svg = val.is_truthy();
                    if (key == "allow_mime" && val.type() == Value::ARRAY) {
                        for (auto& v : *val.as_array()) {
                            if (v.type() != Value::STRING || v.to_string() == "__assoc__") continue;
                            allow_mime.push_back(v.to_string());
                        }
                    }
                }
            }
        }

        // Boyut kontrolü
        if (uf.size > max_size)
            throw std::runtime_error("Uploaded file exceeds max_size limit (" +
                                     std::to_string(max_size) + " bytes)");

        // SVG kontrolü — allow_mime içinde SVG varsa allow_svg de zorunlu
        if (uf.mime == "image/svg+xml" && !allow_svg)
            throw std::runtime_error("SVG upload requires allow_svg: true option");

        // MIME kontrolü — allow_mime belirtilmişse listede olmalı
        if (!allow_mime.empty()) {
            bool found = false;
            for (auto& m : allow_mime) if (m == uf.mime) { found = true; break; }
            if (!found)
                throw std::runtime_error("File type not allowed: " + uf.mime);
        }

        // Dönüş değeri — assoc array
        auto result = std::make_shared<std::vector<Value>>();
        result->push_back(Value(std::string("__assoc__")));
        result->push_back(Value(std::string("path")));   result->push_back(Value(uf.temp_path));
        result->push_back(Value(std::string("mime")));   result->push_back(Value(uf.mime));
        result->push_back(Value(std::string("size")));   result->push_back(Value((int64_t)uf.size));
        result->push_back(Value(std::string("sha256"))); result->push_back(Value(uf.sha256));
        return Value(result);
    };

    return m;
}

// HTTP response header CRLF injection savunması: header adı/değerindeki CR/LF
// (ve diğer kontrol karakterleri) response splitting / Set-Cookie enjeksiyonu /
// open-redirect+CRLF sağlar. İlk CR/LF/NUL'da kes — sonrası enjeksiyon yükü.
// (Serileştirme katmanında da tekrar uygulanır — defense-in-depth.)
static std::string sanitize_header(const std::string& s) {
    size_t cut = s.find_first_of("\r\n");
    std::string v = (cut == std::string::npos) ? s : s.substr(0, cut);
    // Kalan CR/LF/NUL'ları da temizle (paranoya) — normalde cut zaten kesti.
    v.erase(std::remove_if(v.begin(), v.end(),
        [](unsigned char c){ return c == '\r' || c == '\n' || c == '\0'; }), v.end());
    return v;
}

static Module make_response_module(WebContext* ctx) {
    Module m;
    m.name = "response";

    m.functions["status"] = [ctx](auto args) -> Value {
        if (!args.empty()) ctx->set_status(args[0].to_int());
        return Value(ctx->status_code);
    };
    m.functions["header"] = [ctx](auto args) -> Value {
        if (args.size() >= 2)
            ctx->headers_out[sanitize_header(args[0].to_string())] =
                sanitize_header(args[1].to_string());
        return Value();
    };
    m.functions["redirect"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->set_status(args.size() >= 2 ? args[1].to_int() : 302);
        ctx->headers_out["Location"] = sanitize_header(args[0].to_string());
        return Value();
    };
    // response::json/text/html — body'yi ctx->response_body'ye yazar (web.h sözleşmesi).
    // Return değeri expression statement olarak çağrıldığında kaybolur;
    // FCGI/HTTP çıkış katmanları body'yi response_body'den okur.
    m.functions["json"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "application/json; charset=utf-8";
        if (args.size() >= 2) ctx->set_status(args[1].to_int());   // response::json($d, 201)
        if (!args.empty()) {
            std::string s = json_encode(args[0]);
            ctx->response_body += s;
            return Value(s);
        }
        return Value();
    };
    // response::error(kod, mesaj) — durum kodu + {"ok":false,"hata":mesaj} JSON gövdesi.
    // Tek satırda hata dönüşü: return response::error(404, "Bulunamadı")
    // 3 satırlık status+json+return kalıbının yerine geçer.
    m.functions["error"] = [ctx](auto args) -> Value {
        int code = args.empty() ? 500 : args[0].to_int();
        std::string msg = args.size() >= 2 ? args[1].to_string() : "Hata";
        ctx->set_status(code);
        ctx->headers_out["Content-Type"] = "application/json; charset=utf-8";
        auto obj = std::make_shared<std::vector<Value>>();
        obj->push_back(Value(std::string("__assoc__")));
        obj->push_back(Value(std::string("ok")));   obj->push_back(Value(false));
        obj->push_back(Value(std::string("hata")));  obj->push_back(Value(msg));
        std::string s = json_encode(Value(obj));
        ctx->response_body += s;
        return Value(s);
    };
    m.functions["text"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "text/plain; charset=utf-8";
        if (!args.empty()) ctx->response_body += args[0].to_string();
        return Value();
    };
    m.functions["html"] = [ctx](auto args) -> Value {
        ctx->headers_out["Content-Type"] = "text/html; charset=utf-8";
        if (!args.empty()) ctx->response_body += args[0].to_string();
        return Value();
    };

    return m;
}

static Module make_json_module() {
    Module m;
    m.name = "json";

    m.functions["encode"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string("null"));
        return Value(json_encode(args[0]));
    };
    m.functions["decode"] = [](auto args) -> Value {
        if (args.empty()) return Value();
        std::string s = args[0].to_string();
        size_t i = 0;
        try { return json_decode_value(s, i); }
        catch (const JsonTooDeep&) { return Value(); }   // aşırı derin → null (DoS savunması)
        catch (const JsonParseError&) { return Value(); } // bozuk → null (sonsuz döngü DoS savunması)
    };

    return m;
}

// Cerez adi/degeri icin yuzde-kodlama.
//
// ESKI HATA: sanitize_header() yalnizca CR/LF/NUL temizliyordu — genel basliklar
// icin dogru, ama cerezde ';' YAPISAL AYRACTIR. Kullanici kontrolundeki deger
// cereze yazilinca (dil/tema tercihi gibi cok yaygin kalip) saldirgan CEREZ
// OZNITELIKLERINI kontrol edebiliyordu:
//   ?v=x; HttpOnly; Domain=evil.com
//   -> Set-Cookie: tercih=x; HttpOnly; Domain=evil.com; Path=/
// Etki: Domain'i ust alan adina cekip cerezi TUM alt alan adlarina sizdirmak,
// Max-Age ile kalicilastirmak, Path/Secure ile uygulamanin niyetini degistirmek.
// (CRLF enjeksiyonu zaten engelliydi — bu ondan farkli bir kanal.)
//
// Cozum PHP setcookie() ile ayni: yazarken kodla, okurken coz. Mesru veri
// (';' iceren bir deger) gidis-donuste korunur ama yapi anlami tasiyamaz.
static std::string cookie_enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) {
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0xF]; }
    }
    return o;
}
static std::string cookie_dec(const std::string& s) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexval(s[i+1]), l = hexval(s[i+2]);
            if (h >= 0 && l >= 0) { o += (char)((h << 4) | l); i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

static Module make_cookie_module(WebContext* ctx) {
    Module m;
    m.name = "cookie";

    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->cookies_in.find(args[0].to_string());
        if (it != ctx->cookies_in.end()) return Value(cookie_dec(it->second));
        return args.size() >= 2 ? args[1] : Value();   // varsayilan (env kalibi)
    };
    m.functions["set"] = [ctx](auto args) -> Value {
        if (args.size() < 2) return Value();
        // Yuzde-kodla: ';' ve diger yapisal karakterler oznitelik enjekte edemesin.
        std::string name    = cookie_enc(args[0].to_string());
        std::string value   = cookie_enc(args[1].to_string());
        int expires         = args.size() >= 3 ? args[2].to_int() : 0;
        std::string path_c  = sanitize_header(args.size() >= 4 ? args[3].to_string() : "/");

        std::string cookie = name + "=" + value + "; Path=" + path_c;
        if (expires > 0) {
            time_t exp = std::time(nullptr) + expires;
            char buf[64];
            // gmtime paylaşılan statik buffer döndürür (thread race) — gmtime_r
            struct tm tm_buf{};
#ifdef _WIN32
            gmtime_s(&tm_buf, &exp);
#else
            gmtime_r(&exp, &tm_buf);
#endif
            strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
            cookie += "; Expires=" + std::string(buf);
        }
        // Her çerez ayrı Set-Cookie satırı — std::map tek anahtarı ezerdi.
        ctx->set_cookies_out.push_back(cookie);
        return Value();
    };
    m.functions["delete"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        ctx->set_cookies_out.push_back(
            cookie_enc(args[0].to_string()) + "=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
        return Value();
    };
    m.functions["has"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value(false);
        return Value(ctx->cookies_in.count(args[0].to_string()) > 0);
    };

    return m;
}

// ── Session depolama backend'i ────────────────────────────────────────────────
// LOOK_SESSION_DRIVER=redis + LOOK_REDIS_URL → Redis (multi-server session —
// load balancer arkasında session'lar paylaşılır). Aksi halde dosya (tek sunucu).
// Blob formatı iki backend'de de aynı: "key=value\n" satırları.
static bool session_use_redis() {
    static const bool v = []() {
        const char* d = std::getenv("LOOK_SESSION_DRIVER");
        const char* u = std::getenv("LOOK_REDIS_URL");
        return d && std::string(d) == "redis" && u && *u;
    }();
    return v;
}
static int session_ttl() {
    static const int v = []() {
        const char* t = std::getenv("LOOK_SESSION_TTL");
        int n = t ? std::atoi(t) : 0;
        return n > 0 ? n : 86400;   // varsayılan 1 gün
    }();
    return v;
}
// Her worker thread kendi Redis bağlantısını tutar (RespClient thread-safe değil).
static RespClient* session_redis() {
    static thread_local std::unique_ptr<RespClient> cli;
    if (!cli) {
        const char* u = std::getenv("LOOK_REDIS_URL");
        cli = std::make_unique<RespClient>(u ? std::string(u) : std::string("redis://127.0.0.1:6379"));
    }
    return cli.get();
}
// GÜVENLİK: Session ID doğrulama — üretilen format TAM 32 hex karakter. sid
// client'ın LOOK_SESSION cookie'sinden gelir ve doğrudan dosya yoluna girer
// (sess_file_path). Doğrulanmazsa "x/../../../etc/cron.d/pwn" gibi bir değer
// path traversal → arbitrary file read (session::get) ve write/truncate
// (session::set → ofstream trunc) sağlar. Katı allowlist: yalnız 32 hex kabul.
static bool valid_sid(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}
static std::string sess_file_path(const std::string& sid) {
    return std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") + "/look_sess_" + sid;
}
// Session blob'u satir tabanli "anahtar=deger\n" formatinda. Anahtar/deger HIC
// KACIRILMIYORDU: degerin icindeki \n YENI BIR ALAN tanimliyordu.
//
// OLCULEN SALDIRI (uygulama kullanici verisini oturuma yaziyorsa — cok yaygin):
//   session::set("rol", "uye")                    // once yetki
//   session::set("isim", request::get("isim"))    // sonra kullanici verisi
//   ?isim=bob\nrol=admin\nadmin=1
//   -> blob: rol=uye / isim=bob / rol=admin / admin=1
//   -> session::get("admin") = "1"   HIC VAR OLMAYAN YETKI ALANI ENJEKTE EDILDI
// Saldirgan login oncesi bir istekte admin/user_id/is_admin gibi alanlari
// uydurabiliyordu. Anahtarda '=' de veriyi karistiriyordu:
//   set("a=b","X") -> get("a") = "b=X"
//
// Cozum: yaz/oku sirasinda kacir. Degerdeki satir sonu MESRU kullanici verisi
// olabilir (textarea), o yuzden reddetmek degil kacirmak dogru — veri
// gidis-donuste aynen korunur, ayrac anlami tasimaz.
// Geriye donuk uyumlu: eski blob'larda ters bolu yok, unescape kimlik islevi gorur.
static std::string sess_esc(const std::string& s, bool is_key) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        if      (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '='  && is_key) o += "\\e";
        else o += c;
    }
    return o;
}
static std::string sess_unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        if      (n == 'n')  o += '\n';
        else if (n == 'r')  o += '\r';
        else if (n == 'e')  o += '=';
        else if (n == '\\') o += '\\';
        else { o += '\\'; o += n; }
    }
    return o;
}

static bool sess_blob_get(const std::string& blob, const std::string& key, std::string& out) {
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == sess_esc(key, true)) {
            out = sess_unesc(line.substr(eq + 1));
            return true;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}
// key'i güncelle (varsa değiştir, yoksa ekle) — dosya backend'inin "append edip
// get ilk eşleşmeyi döndürme" nedeniyle set'in overwrite etmeme bug'ını da giderir.
static void sess_blob_set(std::string& blob, const std::string& key, const std::string& val) {
    const std::string ek = sess_esc(key, true);    // ayrac anlami tasiyamaz
    const std::string ev = sess_esc(val, false);   // \n enjeksiyonu burada olurdu
    std::string out; size_t pos = 0; bool replaced = false;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
        if (!line.empty()) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) == ek) { out += ek + "=" + ev + "\n"; replaced = true; }
            else out += line + "\n";
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!replaced) out += ek + "=" + ev + "\n";
    blob = out;
}
static std::string sess_load(const std::string& sid) {
    if (session_use_redis()) {
        try { return session_redis()->get("look_sess:" + sid); }
        catch (...) { return ""; }   // Redis erişilemezse crash yerine boş session
    }
    std::ifstream f(sess_file_path(sid));
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void sess_store(const std::string& sid, const std::string& blob) {
    if (session_use_redis()) { try { session_redis()->set("look_sess:" + sid, blob, session_ttl()); } catch (...) {} return; }
    std::ofstream f(sess_file_path(sid), std::ios::trunc); f << blob;   // overwrite
}
static void sess_remove(const std::string& sid) {
    if (session_use_redis()) { try { session_redis()->del("look_sess:" + sid); } catch (...) {} return; }
    std::remove(sess_file_path(sid).c_str());
}
// 32-hex CSPRNG session ID üret (fail-closed) — session::start ve regenerate
// paylaşır. /dev/urandom (Linux) veya rand_s (Windows). Güvenli kaynak yoksa
// zayıf ID yaymak yerine hata fırlat.
static std::string gen_session_id() {
    char id[33];
#if defined(_WIN32)
    for (int i = 0; i < 32; i++) { unsigned int r = 0; rand_s(&r); id[i] = "0123456789abcdef"[r % 16]; }
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    uint8_t bytes[16];
    urandom.read(reinterpret_cast<char*>(bytes), 16);
    if (!urandom || urandom.gcount() != 16)
        throw std::runtime_error("session: güvenli rastgelelik alınamadı (/dev/urandom)");
    for (int i = 0; i < 16; i++) {
        id[i*2]   = "0123456789abcdef"[(bytes[i] >> 4) & 0xf];
        id[i*2+1] = "0123456789abcdef"[bytes[i] & 0xf];
    }
#endif
    id[32] = 0;
    return std::string(id);
}

static Module make_session_module(WebContext* ctx) {
    Module m;
    m.name = "session";

    m.functions["start"] = [ctx](auto) -> Value {
        // Cookie yoksa VEYA client geçersiz/kötü niyetli bir SID gönderdiyse
        // (path traversal denemesi dahil) yeni CSPRNG ID üret — client değerine
        // asla güvenme.
        auto sit = ctx->cookies_in.find("LOOK_SESSION");
        if (sit == ctx->cookies_in.end() || !valid_sid(sit->second)) {
            std::string sid = gen_session_id();
            ctx->cookies_in["LOOK_SESSION"] = sid;
            ctx->set_cookies_out.push_back("LOOK_SESSION=" + sid + "; Path=/; HttpOnly; Secure; SameSite=Lax");
        }
        return Value(ctx->cookies_in["LOOK_SESSION"]);
    };
    // session::regenerate() — session fixation koruması. Login / yetki değişiminde
    // çağrılır: mevcut session verisini KORUYARAK yeni bir SID'e taşır, eskisini
    // siler, yeni cookie set eder. Saldırganın önceden sabitlediği (fixation) SID
    // login sonrası geçersiz kalır. OWASP: privilege change'de SID yenile.
    m.functions["regenerate"] = [ctx](auto) -> Value {
        std::string blob;
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it != ctx->cookies_in.end() && valid_sid(it->second)) {
            blob = sess_load(it->second);   // mevcut veriyi al
            sess_remove(it->second);        // eski session'ı geçersizleştir
        }
        std::string sid = gen_session_id();
        ctx->cookies_in["LOOK_SESSION"] = sid;
        ctx->set_cookies_out.push_back("LOOK_SESSION=" + sid + "; Path=/; HttpOnly; Secure; SameSite=Lax");
        if (!blob.empty()) sess_store(sid, blob);   // veriyi yeni SID'e taşı
        return Value(sid);
    };
    m.functions["set"] = [ctx](auto args) -> Value {
        if (args.size() < 2) return Value();
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it == ctx->cookies_in.end() || !valid_sid(it->second)) return Value();  // traversal guard
        std::string blob = sess_load(it->second);
        sess_blob_set(blob, args[0].to_string(), args[1].to_string());
        sess_store(it->second, blob);
        return Value();
    };
    m.functions["get"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it == ctx->cookies_in.end() || !valid_sid(it->second)) return Value();  // traversal guard
        std::string blob = sess_load(it->second), out;
        if (sess_blob_get(blob, args[0].to_string(), out)) return Value(out);
        return args.size() >= 2 ? args[1] : Value();   // varsayilan (env kalibi)
    };
    // session::has(key) — anahtar oturumda var mı? (docs'ta belgeli; get default'undan
    // ayrı: değeri boş string olan anahtar da VAR sayılır)
    m.functions["has"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value(false);
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it == ctx->cookies_in.end() || !valid_sid(it->second)) return Value(false);
        std::string blob = sess_load(it->second), out;
        return Value(sess_blob_get(blob, args[0].to_string(), out));
    };
    m.functions["destroy"] = [ctx](auto) -> Value {
        auto it = ctx->cookies_in.find("LOOK_SESSION");
        if (it != ctx->cookies_in.end() && valid_sid(it->second)) sess_remove(it->second);
        ctx->cookies_in.erase("LOOK_SESSION");  // Sonraki session::start() yeni ID üretsin
        ctx->set_cookies_out.push_back(
            "LOOK_SESSION=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly; Secure; SameSite=Lax");
        return Value();
    };

    return m;
}

// â”€â”€ Route module â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// route() is handled as a built-in function in the interpreter.
// This module provides route::params() helper.

static Module make_route_module(WebContext* ctx) {
    Module m;
    m.name = "route";

    m.functions["param"] = [ctx](auto args) -> Value {
        if (args.empty()) return Value();
        auto it = ctx->route_params.find(args[0].to_string());
        return it != ctx->route_params.end() ? Value(it->second) : Value();
    };
    m.functions["matched"] = [ctx](auto) -> Value {
        return Value(ctx->route_matched);
    };

    return m;
}

// â”€â”€ db module â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// db::connect("mysql://user:pass@host:port/database")
// db::query($conn, "SELECT ...", [$param1, $param2])
// db::exec($conn, "INSERT ...", [$param1])
// db::escape($conn, $str)
// db::last_id($conn)
// db::affected($conn)

static std::string parse_dsn_part(const std::string& dsn,
                                   std::string& user, std::string& pass,
                                   std::string& host, int& port, std::string& db) {
    // mysql://user:pass@host:port/database
    size_t scheme_end = dsn.find("://");
    if (scheme_end == std::string::npos) throw std::runtime_error("db: invalid DSN format");
    std::string scheme = dsn.substr(0, scheme_end);
    std::string rest   = dsn.substr(scheme_end + 3);

    // user:pass@host:port/db
    size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        size_t colon = userinfo.find(':');
        if (colon != std::string::npos) { user = userinfo.substr(0, colon); pass = userinfo.substr(colon + 1); }
        else user = userinfo;
    }
    size_t slash = rest.find('/');
    std::string hostport = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    db = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = std::stoi(hostport.substr(colon + 1)); }
    else host = hostport;

    return scheme;
}

// ── Connection pool — thread-safe, per-request borrow/return ──────────────────
//
// Fiber-aware acquire: when the pool is exhausted and we're running inside a
// fiber, the fiber yields cooperatively (suspend_current) and is re-queued on
// ITS OWN scheduler by notify_fiber_waiters() when a slot becomes free.
//
// Cross-scheduler safety: with SO_REUSEPORT / per-worker FiberSchedulers, each
// worker has a separate scheduler.  The old per-scheduler pool_waiters_ list
// meant Worker-2's release() could never wake a fiber waiting on Worker-1's
// scheduler.  The global g_fiber_waiters vector stores {fiber, scheduler, pool}
// so release() always calls resume_fiber() on the correct scheduler.

struct ConnPool {
    std::string                                dsn;
    std::vector<std::shared_ptr<DbConnection>> all;
    std::vector<std::shared_ptr<DbConnection>> available;
    std::mutex                                 mtx;
    std::condition_variable                    cv;

    std::shared_ptr<DbConnection> acquire() {
        // Fast path: slot available — no waiting needed
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!available.empty()) {
                auto c = available.back(); available.pop_back();
                return c;
            }
        }

        // Slow path: pool exhausted
        look::Fiber*          fiber = look::Fiber::current();
        look::FiberScheduler* sched = look::get_thread_scheduler();

        if (fiber && sched) {
            // Fiber context: yield cooperatively until a slot is available.
            // Re-enqueue on each wake until we actually grab a connection —
            // handles the rare race where another acquire() takes the slot
            // between notify_fiber_waiters() and this fiber resuming.
            auto shared_fiber = fiber->shared_from_this_fiber();
            while (true) {
                {
                    std::lock_guard<std::mutex> lk(g_fiber_waiters_mtx);
                    g_fiber_waiters.push_back({shared_fiber, sched, this});
                }
                look::FiberScheduler::suspend_current();

                std::lock_guard<std::mutex> lk(mtx);
                if (!available.empty()) {
                    auto c = available.back(); available.pop_back();
                    return c;
                }
                // Slot taken by someone else — yield again
            }
        }

        // Thread context: blocking wait (cv.wait is safe here)
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this] { return !available.empty(); });
        auto c = available.back(); available.pop_back();
        return c;
    }

    void release(std::shared_ptr<DbConnection> c) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            available.push_back(c);
        }
        // Wake one fiber waiter first (cross-scheduler safe)
        notify_fiber_waiters();
        // Then wake one blocking thread waiter
        cv.notify_one();
    }

private:
    // ── Global fiber waiter queue (cross-scheduler safe) ─────────────────────
    // Each entry stores the fiber, its owning scheduler, and the pool it waits
    // on so notify_fiber_waiters() can route the resume to the right thread.
    struct FiberWaiter {
        std::shared_ptr<look::Fiber>  fiber;
        look::FiberScheduler*          sched;
        ConnPool*                      pool;
    };

    static std::mutex               g_fiber_waiters_mtx;
    static std::vector<FiberWaiter> g_fiber_waiters;

    static void notify_fiber_waiters() {
        // Wake the first waiter unconditionally — the fiber's while(true) acquire
        // loop re-queues itself if the slot is stolen before it runs.
        std::lock_guard<std::mutex> lk(g_fiber_waiters_mtx);
        if (!g_fiber_waiters.empty()) {
            auto it = g_fiber_waiters.begin();
            it->sched->resume_fiber(std::move(it->fiber));
            g_fiber_waiters.erase(it);
        }
    }
};

std::mutex               ConnPool::g_fiber_waiters_mtx;
std::vector<ConnPool::FiberWaiter> ConnPool::g_fiber_waiters;

static std::map<std::string, std::shared_ptr<ConnPool>> g_pools;
static std::mutex                                        g_pools_mtx;
static int                                               g_pool_seq  = 0;
static std::atomic<int>                                  g_pool_size{0}; // 0 = auto

// Per-request connection map: pool_key → borrowed conn
//
// Two-level lookup — fiber-aware:
//   1. If running inside a fiber: use fiber's FiberLocal storage
//      → Each fiber has its own connection map, no cross-fiber contamination
//   2. If running in thread context (no active fiber): use thread_local
//      → Backward compatible with non-fiber dispatch path
//
// This replaces the previous single thread_local tl_conns which was unsafe
// when multiple fibers shared a thread (fiber A sets conn, yields, fiber B
// overwrites it — fiber A resumes with wrong connection).

static thread_local std::map<std::string, std::shared_ptr<DbConnection>> tl_conns;

// Returns the active connection map for the current execution context.
// Called on every db::query() — must be fast (no allocation on hot path).
static inline std::map<std::string, std::shared_ptr<DbConnection>>& active_conns() {
    look::Fiber* fiber = look::Fiber::current();
    if (fiber) return fiber->local.conns;   // fiber context — per-fiber storage
    return tl_conns;                         // thread context — legacy path
}

void set_db_pool_size(int n) { g_pool_size.store(n > 0 ? n : 0); }

// Kullanılabilir CPU — cgroup CPU limitini dikkate alır (container/systemd CPUQuota).
// hardware_concurrency() fizikseldir; 8-core makinede 2-CPU cgroup'ta 32 worker
// açmak yerine 8 açar (benchmark'ta bulunan optimum) → kısıtlı ortamda az RAM + doğru CPU.
int available_cpus() {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
#ifdef __linux__
    double c = -1.0;
    // cgroup v2: /sys/fs/cgroup/cpu.max = "quota period" (quota="max" → limit yok)
    {
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string a, b;
        if (f && (f >> a >> b) && a != "max") {
            double q = std::atof(a.c_str());
            double p = b.empty() ? 100000.0 : std::atof(b.c_str());
            if (q > 0 && p > 0) c = q / p;
        }
    }
    // cgroup v1: cpu.cfs_quota_us / cpu.cfs_period_us
    if (c < 0) {
        std::ifstream fq("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us");
        std::ifstream fp("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us");
        long q = -1, p = -1;
        if (fq) fq >> q;
        if (fp) fp >> p;
        if (q > 0 && p > 0) c = (double)q / (double)p;
    }
    if (c > 0) {
        int lim = (int)(c + 0.5);       // en yakın tam CPU'ya yuvarla
        if (lim < 1) lim = 1;
        if (lim < hw) hw = lim;          // fiziksel ile cgroup'un küçüğü
    }
#endif
    return hw;
}

void acquire_thread_connections() {
    // Copy pool list under lock, then release before blocking on acquire().
    // Holding g_pools_mtx during pool->acquire() deadlocks: a releasing thread
    // also needs g_pools_mtx (release_thread_connections), so it can never
    // unblock the waiter.
    std::vector<std::pair<std::string, std::shared_ptr<ConnPool>>> pools_snap;
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        pools_snap.assign(g_pools.begin(), g_pools.end());
    }
    auto& conns = active_conns();
    for (auto& [key, pool] : pools_snap)
        conns[key] = pool->acquire();
}

void release_thread_connections() {
    auto& conns = active_conns();
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        for (auto& [key, conn] : conns)
            if (auto it = g_pools.find(key); it != g_pools.end())
                it->second->release(conn);
    }
    conns.clear();
}

void clear_db_pools() {
    // Hot reload öncesi çağrılır — tüm poolları kapat (exclusive lock altında)
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    g_pools.clear();
    // g_pool_seq sıfırlanmaz: yeni pool'lar yeni key alır, eski closure'lar stale olmaz
    // (route_registry yeniden kurulur zaten)
}

// get_conn — returns the active connection for this request/fiber
static std::shared_ptr<DbConnection> get_conn(const Value& v) {
    if (v.type() != Value::STRING)
        throw std::runtime_error("db: invalid connection handle");
    const std::string& key = v.as_string();
    // Fiber-aware lookup: fiber context → FiberLocal, thread context → thread_local
    auto& conns = active_conns();
    auto tl_it = conns.find(key);
    if (tl_it != conns.end()) return tl_it->second;
    // Fallback: single-thread setup mode or direct-mode scripts
    std::shared_ptr<ConnPool> pool;
    {
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        auto it = g_pools.find(key);
        if (it == g_pools.end())
            throw std::runtime_error("db: connection not found");
        pool = it->second;
    }
    // pool->acquire() is fiber-aware: yields cooperatively if exhausted (fiber
    // context) or blocks the thread (thread context).
    std::shared_ptr<DbConnection> raw = pool->acquire();
    conns[key] = raw;
    return raw;
}

// pool_key — sequential handle string
static std::string make_pool_key() {
    std::lock_guard<std::mutex> lk(g_pools_mtx);
    return "__pool_" + std::to_string(g_pool_seq++);
}

// open one physical connection for a given DSN
static std::shared_ptr<DbConnection> open_one_connection(
        const std::string& dsn, const std::vector<Value>& args) {
    if (dsn.substr(0, 9) == "sqlite://") {
        std::string path = dsn.substr(9);
        auto c = std::make_shared<SqliteClient>();
        c->open(path);
        return c;
    }
    if (dsn.substr(0, 11) == "postgres://"  || dsn.substr(0, 14) == "postgresql://" ||
        dsn.substr(0, 14) == "postgresqls://") {
        std::string user, pass, host = "127.0.0.1", db_name;
        int port = 5432;
        std::string scheme = parse_dsn_part(dsn, user, pass, host, port, db_name);
        // TLS: postgresqls:// şeması VEYA db-adında ?tls=... sorgu paramı.
        // PG'de TLS YENİ → verify DOĞRU VARSAYILAN (postgresqls:// / ?tls=verify → doğrulanmış,
        // ?tls=insecure → şifreli ama doğrulanmamış). mysql/redis'in tersine güvenli başlar.
        bool tls = (scheme == "postgresqls");
        bool tls_verify = true;   // güvenli varsayılan
        { size_t q = db_name.find('?');
          if (q != std::string::npos) {
            std::string query = db_name.substr(q + 1);
            db_name = db_name.substr(0, q);
            if (query.find("tls=insecure") != std::string::npos ||
                query.find("ssl=insecure")  != std::string::npos ||
                query.find("sslmode=require") != std::string::npos) { tls = true; tls_verify = false; }
            else if (query.find("tls=verify") != std::string::npos ||
                     query.find("ssl=verify")  != std::string::npos ||
                     query.find("sslmode=verify-full") != std::string::npos ||
                     query.find("tls=1") != std::string::npos ||
                     query.find("tls=true") != std::string::npos) { tls = true; tls_verify = true; }
          } }
        auto c = std::make_shared<PostgresClient>();
        if (tls) c->set_tls(true, tls_verify);
        c->connect(host, port, user, pass, db_name);
        return c;
    }
    std::string user, pass, host = "127.0.0.1", db_name;
    int port = 3306;
    std::string scheme = parse_dsn_part(dsn, user, pass, host, port, db_name);
    // TLS: mysqls://.../mariadbs:// şeması VEYA db-adında ?tls=1 / ?ssl=... sorgu paramı.
    // ?tls=verify / ?ssl=verify → ayrıca sertifika+hostname doğrulaması (MITM'e karşı).
    bool tls = (scheme == "mysqls" || scheme == "mariadbs");
    bool tls_verify = false;
    { size_t q = db_name.find('?');
      if (q != std::string::npos) {
        std::string query = db_name.substr(q + 1);
        db_name = db_name.substr(0, q);   // sorgu dizisini db adından ayıkla
        if (query.find("tls=verify") != std::string::npos || query.find("ssl=verify") != std::string::npos ||
            query.find("ssl=verify_identity") != std::string::npos) { tls = true; tls_verify = true; }
        else if (query.find("tls=1") != std::string::npos || query.find("tls=true") != std::string::npos ||
                 query.find("ssl=") != std::string::npos)
            tls = true;
      } }
    if (scheme == "mysqls")   scheme = "mysql";
    if (scheme == "mariadbs") scheme = "mariadb";
    if (scheme != "mysql" && scheme != "mariadb")
        throw std::runtime_error("db::connect() unsupported DSN scheme: " + scheme);
    DbConfig cfg;
    cfg.host = host; cfg.port = port; cfg.user = user;
    cfg.password = pass; cfg.database = db_name; cfg.tls = tls; cfg.tls_verify = tls_verify;
    if (args.size() >= 2 && args[1].type() == Value::ARRAY) {
        auto& arr = *args[1].as_array();
        for (size_t i = 1; i + 1 < arr.size(); i += 2) {
            std::string k = arr[i].to_string();
            if (k == "timeout" || k == "connect_timeout") cfg.connect_timeout_ms = arr[i+1].to_int();
            if (k == "query_timeout")                      cfg.query_timeout_ms   = arr[i+1].to_int();
            if (k == "reconnect"  || k == "max_reconnect") cfg.max_reconnect      = arr[i+1].to_int();
        }
    }
    auto c = std::make_shared<MySQLClient>();
    c->connect(cfg);
    return c;
}

// Convert DbRow to LOOK array value
static Value row_to_value(const DbRow& row) {
    auto arr = std::make_shared<std::vector<Value>>();
    for (auto& [k, dv] : row) {
        arr->push_back(Value(dv.str));
    }
    return Value(arr);
}

// Convert DbRow to associative array (Value array of [key, value] pairs)
// Since LOOK doesn't have maps yet, we return as positional array
// matching column order, and also expose ::row_get() helper
// MySQL integer column types
static bool mysql_is_int(uint8_t t) {
    return t == 0x01 || t == 0x02 || t == 0x03 ||
           t == 0x08 || t == 0x09 || t == 0x0D;
}
// MySQL floating-point / decimal column types
static bool mysql_is_float(uint8_t t) {
    return t == 0x04 || t == 0x05 || t == 0xF6;
}
// SQLite type sabitleri
static bool sqlite_is_int(uint8_t t)   { return t == sqlite_type::INTEGER; }
static bool sqlite_is_float(uint8_t t) { return t == sqlite_type::FLOAT; }
static bool sqlite_is_null(uint8_t t)  { return t == sqlite_type::NUL; }
// PostgreSQL type sabitleri
static bool pg_is_int(uint8_t t)       { return t == pg_type::INT; }
static bool pg_is_float(uint8_t t)     { return t == pg_type::FLOAT; }
static bool pg_is_null(uint8_t t)      { return t == pg_type::NUL; }
static bool pg_is_bool(uint8_t t)      { return t == pg_type::BOOL; }

static Value rows_to_value(const std::vector<DbRow>& rows) {
    auto result = std::make_shared<std::vector<Value>>();
    for (const auto& row : rows) {
        // Store as assoc array: ["__assoc__", col0, val0, col1, val1, ...]
        auto row_arr = std::make_shared<std::vector<Value>>();
        row_arr->push_back(Value(std::string("__assoc__")));
        for (auto& [k, dv] : row) {
            row_arr->push_back(Value(k));
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) {
                row_arr->push_back(Value());  // null
            } else if (pg_is_bool(dv.type)) {
                // PostgreSQL BOOL: "t" = true, "f" = false
                row_arr->push_back(Value(dv.str == "t" || dv.str == "true" || dv.str == "1"));
            } else if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { row_arr->push_back(Value((int64_t)std::stoll(dv.str))); }
                catch (...) { row_arr->push_back(Value(dv.str)); }
            } else if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                try { row_arr->push_back(Value(std::stod(dv.str))); }
                catch (...) { row_arr->push_back(Value(dv.str)); }
            } else {
                row_arr->push_back(Value(dv.str));
            }
        }
        result->push_back(Value(row_arr));
    }
    return Value(result);
}

// SQL injection koruması — driver'a göre doğru escape
static std::string escape_str(const std::string& s, const char* driver) {
    if (strcmp(driver, "mysql") == 0 || strcmp(driver, "mariadb") == 0)
        return MySQLClient::escape(s);
    // SQLite ve PostgreSQL: standart SQL, tek tırnak '' ile escape
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else           out += c;
    }
    return out;
}

// Value listesi → DbParam listesi — GERÇEK native binding (bind_params escape-enterpolasyonunun
// yerine). Veri SQL metnine HİÇ girmez; driver protokol-seviye bind eder (sqlite3_bind_* / MySQL
// COM_STMT / PG extended-query). Tip eşlemesi bind_params'ınkiyle aynı. Placeholder SAYIM kontrolü
// artık driver-native (sqlite3_bind_parameter_count / PG-mysql server) → backtick/'#' kör-noktası yok.
static std::vector<DbParam> values_to_db_params(const std::vector<Value>& params) {
    std::vector<DbParam> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        switch (p.type()) {
            case Value::NONE:  out.push_back(DbParam::null());                break;
            case Value::INT:   out.push_back(DbParam::from_int(p.as_int()));  break;
            case Value::FLOAT: {
                double d = p.as_float();
                // NaN/Infinity: sqlite/pg SESSIZCE null yapar, mysql "Out of range" verir —
                // üçü tutarsız + ikisi sessiz-veri-kaybı (Cephe 2). LOOK 0.0/0.0'da fail-loud;
                // DB bind'de de fail-loud + TUTARLI: temiz hata (sessiz-null yerine).
                if (!std::isfinite(d))
                    throw std::runtime_error("db: NaN/Infinity float parametre olarak bind edilemez");
                out.push_back(DbParam::from_float(d));
                break;
            }
            case Value::BOOL:  out.push_back(DbParam::from_bool(p.as_bool())); break;
            default:           out.push_back(DbParam::from_text(p.to_string())); break;
        }
    }
    return out;
}

static Module make_db_module(Interpreter* interp) {
    Module m;
    m.name = "db";

    // db::connect("mysql://user:pass@host/database")
    // db::connect("sqlite://blog.db")
    // Creates a connection pool; pool size = --workers (default: hardware_concurrency)
    m.functions["connect"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::connect() requires DSN string");
        std::string dsn = args[0].to_string();

        // İdempotency: aynı DSN için mevcut pool'un key'ini döndür.
        // Bu olmadan route içindeki db::connect() HER İSTEKTE yeni pool açar
        // (istek başına N bağlantı) → MySQL max_connections tükenir.
        // (Regresyon suite'i tarafından yakalandı: 24 istekte 14'ü başarısız.)
        static std::map<std::string, std::string> dsn_to_key;  // g_pools_mtx korur
        {
            std::lock_guard<std::mutex> lk(g_pools_mtx);
            auto it = dsn_to_key.find(dsn);
            if (it != dsn_to_key.end() && g_pools.count(it->second))
                return Value(it->second);
        }

        bool is_sqlite = (dsn.substr(0, 9) == "sqlite://");
        int sz = g_pool_size.load();
        if (sz <= 0) sz = look::available_cpus();
        if (sz < 2)  sz = 2;
        if (is_sqlite) { if (sz > 4) sz = 4; if (sz < 2) sz = 2; }  // SQLite: WAL concurrent reads, up to 4

        // SQLite `:memory:` her bağlantıya özeldir — çok bağlantılı pool'da
        // (ve her worker thread'in kendi bağlantısını sabitlediği modelde) setup'ta
        // bir bağlantıda yaratılan tablo diğerlerinde YOK ("no such table").
        // Çözüm: :memory:'yi süreç-ömürlü geçici bir DOSYAYA yönlendir — tüm pool
        // bağlantıları aynı DB'yi paylaşır (tablo görünür), pool=N korunur. Thread
        // güvenliği per-connection ayrımdan DEĞİL, SQLITE_THREADSAFE=2'nin koruduğu
        // global statiklerden gelir (bkz. CMakeLists; eski =0 yorumu yanlıştı).
        // Kullanıcının :memory:'den beklediği "tek paylaşımlı geçici DB" davranışı budur.
        // İdempotency anahtarı ORİJİNAL dsn kalır; sadece açılan yol değişir.
        // S3: yol süreç-ömürlü ÖZEL bir dizinin içinde. Eski look_mem_<pid>.db yolu
        // öngörülebilir + dünya-yazılabilir /tmp'deydi → yerel saldırgan önceden symlink
        // koyup TOCTOU (std::remove sonra open) ile başka dosyaya yazdırabilirdi (paylaşımlı
        // hosting). Fix: dizini mkdtemp (POSIX, atomik 0700 + yalnız-sahip) / tahmin-edilemez
        // ad (Windows, temp zaten kullanıcıya-özel ACL'li) ile aç, DB'yi İÇİNE koy. Dizin
        // bize ait ve 0700 olduğu için içindeki dosya O_EXCL|O_NOFOLLOW gerektirmez —
        // saldırgan var olmayan özel dizine symlink koyamaz. Bir kez oluştur, pool paylaşır.
        std::string effective_dsn = dsn;
        if (is_sqlite && dsn.find(":memory:") != std::string::npos) {
            static std::mutex memdb_mtx;
            static std::string memdb_path;
            {
                std::lock_guard<std::mutex> lk(memdb_mtx);
                if (memdb_path.empty()) {
                    std::error_code ec;
                    auto tmp = std::filesystem::temp_directory_path(ec);
#ifdef _WIN32
                    std::random_device rd;
                    char nm[48];
                    std::snprintf(nm, sizeof(nm), "look_mem_%08x%08x%08x",
                                  (unsigned)rd(), (unsigned)rd(), (unsigned)rd());
                    auto dir = tmp / nm;
                    if (!std::filesystem::create_directory(dir, ec))
                        throw std::runtime_error(":memory: icin guvenli gecici dizin acilamadi");
                    memdb_path = (dir / "db.sqlite").string();
#else
                    std::string tpl = (tmp / "look_mem_XXXXXX").string();
                    if (::mkdtemp(tpl.data()) == nullptr)
                        throw std::runtime_error(":memory: icin guvenli gecici dizin acilamadi (mkdtemp)");
                    memdb_path = tpl + "/db.sqlite";
#endif
                }
            }
            effective_dsn = "sqlite://" + memdb_path;
            Logger::instance().log(LogLevel::LOG_INFO, "DB",
                ":memory: → süreç-ömürlü ÖZEL (0700) geçici dosyaya yönlendirildi");
        }

        auto pool = std::make_shared<ConnPool>();
        pool->dsn = effective_dsn;
        for (int i = 0; i < sz; i++) {
            auto c = open_one_connection(effective_dsn, args);
            pool->all.push_back(c);
            pool->available.push_back(c);
        }

        std::string key = make_pool_key();
        {
            std::lock_guard<std::mutex> lk(g_pools_mtx);
            // Yarış: başka thread aynı DSN'i bizden önce kaydettiyse onunkini
            // kullan — bizim yeni açtığımız bağlantılar scope sonunda kapanır.
            auto it = dsn_to_key.find(dsn);
            if (it != dsn_to_key.end() && g_pools.count(it->second))
                return Value(it->second);
            g_pools[key]    = pool;
            dsn_to_key[dsn] = key;
        }
        Logger::instance().log(LogLevel::LOG_INFO, "DB",
            "Pool[" + std::to_string(sz) + "] created: " +
            dsn.substr(0, dsn.find('@') == std::string::npos ? dsn.size() : dsn.find('@')));

        // S4 Faz 1: DB-TLS güvenlik uyarısı — POOL BASINA BİR KEZ (sorgu başına değil → spam yok).
        // El-yazması wire parser'a ağdaki herkes bayt besliyor; kullanıcı şifreleme/doğrulama
        // durumunu bilmeli ve NE YAPACAĞINI görmeli. Davranış değişmez (yalnız log). Faz 2: verify
        // varsayılan + ?tls=insecure opt-out (sürüm kapısı, ayrı karar).
        {
            std::string sch = dsn.substr(0, dsn.find(':'));
            bool has_tls = dsn.find("tls=") != std::string::npos || dsn.find("ssl=") != std::string::npos;
            bool vfy     = dsn.find("tls=verify") != std::string::npos || dsn.find("ssl=verify") != std::string::npos;
            bool secure_sch = (sch == "mysqls" || sch == "mariadbs" || sch == "rediss");
            bool encrypted  = secure_sch ||
                (has_tls && (sch == "mysql" || sch == "mariadb" || sch == "redis"));
            std::string w;
            // PostgreSQL: TLS artik VAR (postgresqls:// / ?tls=). verify DOGRU VARSAYILAN →
            // yalniz ACIK plaintext'te veya insecure opt-out'ta uyar.
            bool pg = (sch == "postgres" || sch == "postgresql" || sch == "postgresqls");
            bool pg_encrypted = (sch == "postgresqls") ||
                (has_tls && (dsn.find("tls=") != std::string::npos || dsn.find("sslmode=") != std::string::npos));
            bool pg_insecure = dsn.find("tls=insecure") != std::string::npos ||
                               dsn.find("ssl=insecure")  != std::string::npos ||
                               dsn.find("sslmode=require") != std::string::npos;
            if (pg) {
                if (!pg_encrypted)
                    w = "PostgreSQL baglantisi SIFRELENMEMIS (plaintext) — sifreli+dogrulanmis icin: postgresqls://... veya DSN'e ?tls=verify.";
                else if (pg_insecure)
                    w = "PostgreSQL TLS sertifikasi DOGRULANMIYOR (MITM riski) — ?tls=insecure yerine postgresqls:// veya ?tls=verify kullanin.";
                // aksi halde postgresqls:// / ?tls=verify → sifreli+dogrulanmis → uyari yok
            }
            else if ((sch == "mysql" || sch == "mariadb" || secure_sch) && !encrypted && sch[0] == 'm')
                w = "MySQL baglantisi SIFRELENMEMIS (plaintext) — sifreli+dogrulanmis icin: mysqls://... veya DSN'e ?tls=verify.";
            else if ((sch == "redis" || sch == "rediss") && !encrypted)
                w = "Redis baglantisi SIFRELENMEMIS (plaintext) — sifreli icin rediss://, dogrulanmis icin ?tls=verify.";
            else if (encrypted && !vfy)
                w = std::string(sch[0] == 'r' ? "Redis" : "MySQL") +
                    " TLS sertifikasi DOGRULANMIYOR (MITM riski) — DSN'e ?tls=verify ekleyin.";
            if (!w.empty())
                Logger::instance().log(LogLevel::LOG_WARN, "DB-TLS", w);
        }
        return Value(key);
    };

    // db::query($conn, "SELECT ...", [$p1, $p2])
    m.functions["query"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::query() requires connection and SQL");
        auto conn = get_conn(args[0]);
        std::string sql = args[1].to_string();
        std::vector<Value> params;
        if (args.size() >= 3 && args[2].type() == Value::ARRAY)
            params = *args[2].as_array();
        auto rows = conn->execute(sql, values_to_db_params(params));   // gerçek native binding
        return rows_to_value(rows);
    };

    // db::exec($conn, "INSERT/UPDATE/DELETE ...", [$p1])
    m.functions["exec"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::exec() requires connection and SQL");
        auto conn = get_conn(args[0]);
        std::string sql = args[1].to_string();
        std::vector<Value> params;
        if (args.size() >= 3 && args[2].type() == Value::ARRAY)
            params = *args[2].as_array();
        conn->execute(sql, values_to_db_params(params));   // gerçek native binding
        return Value((int64_t)conn->affected_rows());
    };

    // db::last_id($conn) — uses thread-local connection (same as exec in this request)
    m.functions["last_id"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::last_id() requires connection");
        return Value((int64_t)get_conn(args[0])->last_insert_id());
    };

    // db::affected($conn)
    m.functions["affected"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::affected() requires connection");
        return Value((int64_t)get_conn(args[0])->affected_rows());
    };

    // db::escape($conn, $str) — DRIVER-AWARE. Eskiden HER sürücü için MySQL
    // backslash escaping (`\'`) kullanıyordu; ama SQLite/PostgreSQL standart
    // SQL'de backslash'i escape saymaz → `\'` içindeki `'` string'i kapatıp
    // INJECTION açardı. escape_str sürücüye göre doğru escaper'ı seçer
    // (MySQL: backslash; SQLite/PG: `''` ikilemesi).
    m.functions["escape"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::escape() requires connection and string");
        return Value(escape_str(args[1].to_string(), get_conn(args[0])->driver_name()));
    };

    // db::close($conn) — removes the pool; existing borrowed conns complete normally
    m.functions["close"] = [](auto args) -> Value {
        if (args.empty()) return Value();
        std::lock_guard<std::mutex> lk(g_pools_mtx);
        g_pools.erase(args[0].to_string());
        return Value();
    };

    // db::begin/commit/rollback — manuel transaction kontrolü
    m.functions["begin"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::begin() requires connection");
        auto c = get_conn(args[0]);
        c->query(c->begin_stmt());   // SQLite: BEGIN IMMEDIATE (D-01)
        return Value();
    };
    m.functions["commit"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::commit() requires connection");
        get_conn(args[0])->query("COMMIT");
        return Value();
    };
    m.functions["rollback"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("db::rollback() requires connection");
        get_conn(args[0])->query("ROLLBACK");
        return Value();
    };

    // db::transaction($conn, fn) — nested SAVEPOINT destekli wrapper
    m.functions["transaction"] = [interp](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("db::transaction() requires connection and function");
        auto conn = get_conn(args[0]);
        const bool nested = (conn->tx_depth > 0);
        const std::string sp = nested ? ("look_sp_" + std::to_string(conn->tx_depth)) : "";
        if (nested) {
            conn->query("SAVEPOINT " + sp);
        } else {
            conn->query(conn->begin_stmt());   // SQLite: BEGIN IMMEDIATE (D-01)
        }
        conn->tx_depth++;
        try {
            // invoke() FUNCTION'ı da VM closure'ını da (BYTECODE_FN → köprü) ele alır.
            // Eski "sadece FUNCTION" koruması VM closure'ını SESSİZCE atlayıp COMMIT
            // ediyordu (transaction gövdesi hiç çalışmadan commit → veri kaybı). Şu an
            // fallback maskeliyor (db::transaction builtin değildi) ama builtin olarak
            // eklenince tetiklenirdi. Fonksiyon olmayan argüman invoke içinde hata
            // fırlatır → aşağıdaki catch → ROLLBACK (sessiz commit yerine).
            Value result = interp->invoke(args[1], {});
            conn->tx_depth--;
            if (nested) {
                conn->query("RELEASE SAVEPOINT " + sp);
            } else {
                conn->query("COMMIT");
            }
            return result;
        } catch (...) {
            conn->tx_depth--;
            try {
                if (nested) {
                    conn->query("ROLLBACK TO SAVEPOINT " + sp);
                    conn->query("RELEASE SAVEPOINT " + sp);
                } else {
                    conn->query("ROLLBACK");
                }
            } catch (...) {}
            throw;
        }
    };

    // db::col($conn, $sql, $params) → ilk satırın ilk kolonu
    // db::col($row, $index_or_name) → row'dan kolon değeri
    m.functions["col"] = [](auto args) -> Value {
        if (args.size() < 2) return Value();

        // Bağlantı + SQL çalıştırma modu
        auto is_pool_handle = [](const Value& v) {
            if (v.type() != Value::STRING) return false;
            const auto& s = v.as_string();
            return s.substr(0, 7) == "__pool_";
        };
        if (is_pool_handle(args[0])) {
            auto conn = get_conn(args[0]);
            std::string sql = args[1].to_string();
            std::vector<Value> params;
            if (args.size() >= 3 && args[2].type() == Value::ARRAY)
                params = *args[2].as_array();
            auto rows = conn->execute(sql, values_to_db_params(params));   // gerçek native binding
            if (rows.empty() || rows[0].empty()) return Value();
            // İlk satırın ilk kolonunu dön
            const auto& dv = rows[0][0].second;
            if (dv.is_null || sqlite_is_null(dv.type) || pg_is_null(dv.type)) return Value();
            if (mysql_is_int(dv.type) || sqlite_is_int(dv.type) || pg_is_int(dv.type)) {
                try { return Value((int64_t)std::stoll(dv.str)); } catch (...) {}
            }
            if (mysql_is_float(dv.type) || sqlite_is_float(dv.type) || pg_is_float(dv.type)) {
                try { return Value(std::stod(dv.str)); } catch (...) {}
            }
            return Value(dv.str);
        }

        if (args[0].type() != Value::ARRAY) return Value();
        auto& arr = *args[0].as_array();

        // Assoc array row: ["__assoc__", col0, val0, col1, val1, ...]
        if (!arr.empty() && arr[0].type() == Value::STRING &&
            arr[0].as_string() == "__assoc__") {
            // String key: find by name
            if (args[1].type() == Value::STRING) {
                const std::string& key = args[1].as_string();
                for (size_t i = 1; i + 1 < arr.size(); i += 2)
                    if (arr[i].to_string() == key) return arr[i + 1];
                return Value();
            }
            // Numeric index: 0 = first column value
            int idx = args[1].to_int();
            size_t val_pos = 2 + idx * 2; // skip __assoc__ + key, land on value
            if (val_pos < arr.size()) return arr[val_pos];
            return Value();
        }

        // Plain array
        int idx = args[1].to_int();
        if (idx < 0 || idx >= (int)arr.size()) return Value();
        return arr[idx];
    };

    return m;
}

// â”€â”€ Export â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::map<std::string, Module> make_web_modules(WebContext* ctx, Interpreter* interp) {
    std::map<std::string, Module> mods;
    auto add = [&](Module mod) { mods[mod.name] = std::move(mod); };
    add(make_request(ctx));
    add(make_response_module(ctx));
    add(make_json_module());
    add(make_cookie_module(ctx));
    add(make_session_module(ctx));
    add(make_route_module(ctx));
    add(make_db_module(interp));
    return mods;
}

} // namespace look


#include "look/stdlib.h"
#include "look/interpreter.h"
#include "look/logger.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <regex>
#include <cstring>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
  #ifdef ERROR
  #undef ERROR
  #endif
#else
  #include <fstream>
#endif

namespace look {

// ── Base64 (auth icin) ────────────────────────────────────────────────────────

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const uint8_t* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = ((uint32_t)data[i] << 16) |
                     (i+1 < len ? (uint32_t)data[i+1] << 8 : 0) |
                     (i+2 < len ? (uint32_t)data[i+2] : 0);
        out += B64[(b >> 18) & 0x3F];
        out += B64[(b >> 12) & 0x3F];
        out += (i+1 < len) ? B64[(b >> 6) & 0x3F] : '=';
        out += (i+2 < len) ? B64[b & 0x3F] : '=';
    }
    return out;
}

static std::vector<uint8_t> b64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 3 < s.size(); i += 4) {
        int a = val(s[i]), b = val(s[i+1]), c = val(s[i+2]), d = val(s[i+3]);
        if (a<0||b<0) break;
        out.push_back((a<<2)|(b>>4));
        if (c>=0) out.push_back((b<<4)|(c>>2));
        if (d>=0) out.push_back((c<<6)|d);
    }
    return out;
}

// ── Pure C++ SHA-256 ──────────────────────────────────────────────────────────

static std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

    std::vector<uint8_t> msg(data);
    uint64_t bit_len=(uint64_t)data.size()*8;
    msg.push_back(0x80);
    while(msg.size()%64!=56) msg.push_back(0);
    for(int i=7;i>=0;i--) msg.push_back((uint8_t)(bit_len>>(i*8)));

    auto rotr=[](uint32_t x,int n){return(x>>n)|(x<<(32-n));};
    for(size_t i=0;i<msg.size();i+=64){
        uint32_t w[64];
        for(int j=0;j<16;j++)
            w[j]=((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                 ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for(int j=16;j<64;j++){
            uint32_t s0=rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
            uint32_t s1=rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
            w[j]=w[j-16]+s0+w[j-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int j=0;j<64;j++){
            uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t t1=hh+S1+ch+K[j]+w[j];
            uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::vector<uint8_t> r(32);
    for(int i=0;i<8;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
    return r;
}

// ── HMAC-SHA256 ───────────────────────────────────────────────────────────────

static std::vector<uint8_t> hmac_sha256(
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& msg)
{
    static const size_t BS = 64;
    std::vector<uint8_t> k = key;
    if (k.size() > BS) k = sha256(k);
    k.resize(BS, 0);
    std::vector<uint8_t> o_key(BS), i_key(BS);
    for(size_t i=0;i<BS;i++){o_key[i]=k[i]^0x5c;i_key[i]=k[i]^0x36;}
    std::vector<uint8_t> inner(i_key);
    inner.insert(inner.end(), msg.begin(), msg.end());
    auto ih = sha256(inner);
    std::vector<uint8_t> outer(o_key);
    outer.insert(outer.end(), ih.begin(), ih.end());
    return sha256(outer);
}

// ── PBKDF2-HMAC-SHA256 — platform bagimsiz ───────────────────────────────────

static std::vector<uint8_t> pbkdf2_sha256(
    const std::string& password, const std::vector<uint8_t>& salt,
    uint32_t iterations, uint32_t dk_len)
{
    std::vector<uint8_t> pwd(password.begin(), password.end());
    std::vector<uint8_t> dk;
    uint32_t blocks = (dk_len + 31) / 32;
    for (uint32_t block = 1; block <= blocks; block++) {
        std::vector<uint8_t> sb = salt;
        sb.push_back((block>>24)&0xFF); sb.push_back((block>>16)&0xFF);
        sb.push_back((block>>8)&0xFF);  sb.push_back(block&0xFF);
        auto u = hmac_sha256(pwd, sb);
        auto f = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = hmac_sha256(pwd, u);
            for (size_t j = 0; j < f.size(); j++) f[j] ^= u[j];
        }
        dk.insert(dk.end(), f.begin(), f.end());
    }
    dk.resize(dk_len);
    return dk;
}

// ── Kriptografik rastgele bayt — cross-platform ───────────────────────────────

static std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> buf(n);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE rng = nullptr;
    BCryptOpenAlgorithmProvider(&rng, BCRYPT_RNG_ALGORITHM, nullptr, 0);
    BCryptGenRandom(rng, buf.data(), (ULONG)n, 0);
    BCryptCloseAlgorithmProvider(rng, 0);
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) throw std::runtime_error("auth: /dev/urandom acilamadi");
    urandom.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)n);
    if (!urandom) throw std::runtime_error("auth: yeterli rastgele bayt okunamadi");
#endif
    return buf;
}

// ── auth module ───────────────────────────────────────────────────────────────

static Module make_auth() {
    Module m;
    m.name = "auth";

    // auth::hash("password") → "pbkdf2$sha256$100000$salt_b64$hash_b64"
    m.functions["hash"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("auth::hash() requires password");
        std::string pwd = args[0].to_string();
        uint32_t iter = args.size() >= 2 ? (uint32_t)args[1].to_int() : 100000;

        auto salt = random_bytes(32);
        auto dk   = pbkdf2_sha256(pwd, salt, iter, 32);

        std::string result = "pbkdf2$sha256$" + std::to_string(iter) + "$"
                           + b64_encode(salt.data(), salt.size()) + "$"
                           + b64_encode(dk.data(), dk.size());
        return Value(result);
    };

    // auth::verify("password", "pbkdf2$...") → true/false
    m.functions["verify"] = [](auto args) -> Value {
        if (args.size() < 2) return Value(false);
        std::string pwd  = args[0].to_string();
        std::string hash = args[1].to_string();

        // Parse: pbkdf2$sha256$iterations$salt_b64$hash_b64
        std::vector<std::string> parts;
        std::istringstream ss(hash);
        std::string tok;
        while (std::getline(ss, tok, '$')) parts.push_back(tok);

        if (parts.size() < 5 || parts[0] != "pbkdf2") return Value(false);

        uint32_t iter    = (uint32_t)std::stoi(parts[2]);
        auto     salt    = b64_decode(parts[3]);
        auto     stored  = b64_decode(parts[4]);
        auto     derived = pbkdf2_sha256(pwd, salt, iter, 32);

        // Constant-time compare
        if (derived.size() != stored.size()) return Value(false);
        uint8_t diff = 0;
        for (size_t i = 0; i < derived.size(); i++) diff |= derived[i] ^ stored[i];
        return Value(diff == 0);
    };

    return m;
}

// ── validator module ──────────────────────────────────────────────────────────
// validator::check($data, ["email" => ["required","email"], "yas" => ["integer","min:0"]])
// Returns: ["ok" => bool, "errors" => ["field" => "message", ...]]

static std::string get_field(const Value& data, const std::string& key) {
    if (data.type() != Value::ARRAY) return "";
    auto& arr = *data.as_array();
    if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__") {
        for (size_t i = 1; i + 1 < arr.size(); i += 2)
            if (arr[i].to_string() == key) return arr[i+1].to_string();
    }
    return "";
}

static bool is_email(const std::string& s) {
    static const std::regex re(R"([a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,})");
    return std::regex_match(s, re);
}

static Module make_validator() {
    Module m;
    m.name = "validator";

    m.functions["check"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("validator::check() requires data and rules");

        const Value& data  = args[0];
        const Value& rules = args[1];

        auto errors_arr = std::make_shared<std::vector<Value>>();
        errors_arr->push_back(Value(std::string("__assoc__")));

        bool ok = true;

        // rules is assoc array: ["field" => ["rule1", "rule2"]]
        if (rules.type() != Value::ARRAY) return Value(false);
        auto& rules_arr = *rules.as_array();

        size_t start = (!rules_arr.empty() && rules_arr[0].as_string() == "__assoc__") ? 1 : 0;

        for (size_t i = start; i + 1 < rules_arr.size(); i += 2) {
            std::string field = rules_arr[i].to_string();
            std::string value = get_field(data, field);
            const Value& field_rules = rules_arr[i+1];

            std::vector<std::string> rule_list;
            if (field_rules.type() == Value::ARRAY) {
                for (auto& r : *field_rules.as_array())
                    if (r.type() != Value::NONE) rule_list.push_back(r.to_string());
            } else {
                rule_list.push_back(field_rules.to_string());
            }

            std::string error;
            for (const auto& rule : rule_list) {
                if (!error.empty()) break;

                if (rule == "required" && value.empty()) {
                    error = field + " zorunlu";
                } else if (rule == "email" && !value.empty() && !is_email(value)) {
                    error = field + " gecersiz e-posta";
                } else if (rule == "integer" && !value.empty()) {
                    try { std::stoi(value); } catch(...) { error = field + " tam sayi olmali"; }
                } else if (rule == "numeric" && !value.empty()) {
                    try { std::stod(value); } catch(...) { error = field + " sayi olmali"; }
                } else if (rule.substr(0,4) == "min:" && !value.empty()) {
                    int minv = std::stoi(rule.substr(4));
                    try {
                        if (std::stoi(value) < minv)
                            error = field + " en az " + std::to_string(minv) + " olmali";
                    } catch(...) {
                        if ((int)value.size() < minv)
                            error = field + " en az " + std::to_string(minv) + " karakter olmali";
                    }
                } else if (rule.substr(0,4) == "max:" && !value.empty()) {
                    int maxv = std::stoi(rule.substr(4));
                    try {
                        if (std::stoi(value) > maxv)
                            error = field + " en fazla " + std::to_string(maxv) + " olmali";
                    } catch(...) {
                        if ((int)value.size() > maxv)
                            error = field + " en fazla " + std::to_string(maxv) + " karakter olmali";
                    }
                } else if (rule.substr(0, 3) == "in:" && !value.empty()) {
                    // "in:admin,user,guest"
                    std::string options = rule.substr(3);
                    std::istringstream iss(options);
                    std::string opt;
                    bool found = false;
                    while (std::getline(iss, opt, ','))
                        if (opt == value) { found = true; break; }
                    if (!found) error = field + " gecersiz deger";
                }
            }

            if (!error.empty()) {
                ok = false;
                errors_arr->push_back(Value(field));
                errors_arr->push_back(Value(error));
            }
        }

        auto result = std::make_shared<std::vector<Value>>();
        result->push_back(Value(std::string("__assoc__")));
        result->push_back(Value(std::string("ok")));
        result->push_back(Value(ok));
        result->push_back(Value(std::string("errors")));
        result->push_back(Value(errors_arr));
        return Value(result);
    };

    return m;
}

// ── array module ──────────────────────────────────────────────────────────────

Module make_array_module(Interpreter* interp) {
    Module m;
    m.name = "array";

    // push($arr, $val) → yeni array döner
    m.functions["push"] = [](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::push() requires array and value");
        auto result = std::make_shared<std::vector<Value>>(*args[0].as_array());
        result->push_back(args[1]);
        return Value(result);
    };

    // pop($arr) → son elemanı döner
    m.functions["pop"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::pop() requires array");
        auto& arr = *args[0].as_array();
        if (arr.empty()) return Value();
        return arr.back();
    };

    // sort($arr) veya sort($arr, function($a,$b){return $a<=>$b;})
    m.functions["sort"] = [interp](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("array::sort() requires array");
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::sort() requires array as first argument");
        auto result = std::make_shared<std::vector<Value>>(*args[0].as_array());
        if (args.size() >= 2 && args[1].type() == Value::FUNCTION) {
            auto fn = args[1];
            std::sort(result->begin(), result->end(), [&](const Value& a, const Value& b) {
                Value r = interp->invoke(fn, {a, b});
                return r.to_int() < 0;
            });
        } else {
            std::sort(result->begin(), result->end(), [](const Value& a, const Value& b) {
                return a < b;
            });
        }
        return Value(result);
    };

    // filter($arr, function($x) { return $x > 3; })
    m.functions["filter"] = [interp](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("array::filter() requires array and callback");
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::filter() requires array");
        auto result = std::make_shared<std::vector<Value>>();
        auto fn = args[1];
        for (auto& elem : *args[0].as_array()) {
            Value keep = interp->invoke(fn, {elem});
            if (keep.is_truthy()) result->push_back(elem);
        }
        return Value(result);
    };

    // map($arr, function($x) { return $x * 2; })
    m.functions["map"] = [interp](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("array::map() requires array and callback");
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::map() requires array");
        auto result = std::make_shared<std::vector<Value>>();
        auto fn = args[1];
        for (auto& elem : *args[0].as_array())
            result->push_back(interp->invoke(fn, {elem}));
        return Value(result);
    };

    // reduce($arr, function($acc, $x) { return $acc + $x; }, $initial)
    m.functions["reduce"] = [interp](auto args) -> Value {
        if (args.size() < 3) throw std::runtime_error("array::reduce() requires array, callback, initial");
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::reduce() requires array");
        auto fn  = args[1];
        Value acc = args[2];
        for (auto& elem : *args[0].as_array())
            acc = interp->invoke(fn, {acc, elem});
        return acc;
    };

    // slice($arr, offset, length)
    m.functions["slice"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("array::slice() requires array");
        if (args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::slice() requires array");
        auto& src = *args[0].as_array();
        int offset = args.size() >= 2 ? args[1].to_int() : 0;
        int length = args.size() >= 3 ? args[2].to_int() : (int)src.size();
        if (offset < 0) offset = (int)src.size() + offset;
        if (offset > (int)src.size()) offset = (int)src.size();
        int end = offset + length;
        if (end > (int)src.size()) end = (int)src.size();
        auto result = std::make_shared<std::vector<Value>>(src.begin() + offset, src.begin() + end);
        return Value(result);
    };

    // contains($arr, $val)
    m.functions["contains"] = [](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY) return Value(false);
        for (auto& elem : *args[0].as_array())
            if (elem == args[1]) return Value(true);
        return Value(false);
    };

    // unique($arr)
    m.functions["unique"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY) return Value(std::make_shared<std::vector<Value>>());
        auto result = std::make_shared<std::vector<Value>>();
        for (auto& elem : *args[0].as_array()) {
            bool found = false;
            for (auto& r : *result) if (r == elem) { found = true; break; }
            if (!found) result->push_back(elem);
        }
        return Value(result);
    };

    // reverse($arr)
    m.functions["reverse"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY) return Value(std::make_shared<std::vector<Value>>());
        auto result = std::make_shared<std::vector<Value>>(*args[0].as_array());
        std::reverse(result->begin(), result->end());
        return Value(result);
    };

    // keys($assoc) → array of keys
    m.functions["keys"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY) return Value(std::make_shared<std::vector<Value>>());
        auto& arr = *args[0].as_array();
        auto result = std::make_shared<std::vector<Value>>();
        if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__") {
            for (size_t i = 1; i + 1 < arr.size(); i += 2) result->push_back(arr[i]);
        } else {
            for (size_t i = 0; i < arr.size(); i++) result->push_back(Value((int)i));
        }
        return Value(result);
    };

    // values($assoc) → array of values
    m.functions["values"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY) return Value(std::make_shared<std::vector<Value>>());
        auto& arr = *args[0].as_array();
        auto result = std::make_shared<std::vector<Value>>();
        if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__") {
            for (size_t i = 2; i < arr.size(); i += 2) result->push_back(arr[i]);
        } else {
            *result = arr;
        }
        return Value(result);
    };

    // find($arr, function($x) { return $x > 3; }) → ilk eşleşen eleman, null döner bulamazsa
    m.functions["find"] = [interp](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::find() requires array and callback");
        auto& arr = *args[0].as_array();
        auto fn = args[1];
        // assoc array: değerler üzerinde iterate
        bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__";
        if (is_assoc) {
            for (size_t i = 1; i + 1 < arr.size(); i += 2) {
                if (interp->invoke(fn, {arr[i+1]}).is_truthy()) return arr[i+1];
            }
        } else {
            for (auto& elem : arr)
                if (interp->invoke(fn, {elem}).is_truthy()) return elem;
        }
        return Value(); // null
    };

    // any($arr, function($x) { return $x > 0; }) → en az biri eşleşirse true
    m.functions["any"] = [interp](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::any() requires array and callback");
        auto& arr = *args[0].as_array();
        auto fn = args[1];
        bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__";
        if (is_assoc) {
            for (size_t i = 1; i + 1 < arr.size(); i += 2)
                if (interp->invoke(fn, {arr[i+1]}).is_truthy()) return Value(true);
        } else {
            for (auto& elem : arr)
                if (interp->invoke(fn, {elem}).is_truthy()) return Value(true);
        }
        return Value(false);
    };

    // all($arr, function($x) { return $x > 0; }) → hepsi eşleşirse true
    m.functions["all"] = [interp](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::all() requires array and callback");
        auto& arr = *args[0].as_array();
        auto fn = args[1];
        bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__";
        if (is_assoc) {
            for (size_t i = 1; i + 1 < arr.size(); i += 2)
                if (!interp->invoke(fn, {arr[i+1]}).is_truthy()) return Value(false);
        } else {
            for (auto& elem : arr)
                if (!interp->invoke(fn, {elem}).is_truthy()) return Value(false);
        }
        return Value(true);
    };

    // flatten($arr [, $depth]) → düzleştirilmiş dizi
    m.functions["flatten"] = [](auto args) -> Value {
        if (args.empty() || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::flatten() requires array");
        int depth = args.size() >= 2 ? args[1].to_int() : -1; // -1 = sonsuz
        auto result = std::make_shared<std::vector<Value>>();
        // do_flatten: v bir alt eleman — kendisi array ise depth kadar daha açar
        std::function<void(const Value&, int)> do_flatten = [&](const Value& v, int d) {
            if (v.type() != Value::ARRAY) { result->push_back(v); return; }
            auto& arr = *v.as_array();
            bool is_assoc = !arr.empty() && arr[0].type() == Value::STRING &&
                            arr[0].as_string() == "__assoc__";
            if (is_assoc || d == 0) { result->push_back(v); return; }
            for (auto& elem : arr)
                do_flatten(elem, d > 0 ? d - 1 : -1);
        };
        // Dış dizi her zaman açılır; her eleman depth ile kontrol edilir
        for (auto& elem : *args[0].as_array())
            do_flatten(elem, depth);
        return Value(result);
    };

    // chunk($arr, $size) → [[a,b],[c,d],...] şeklinde parçalar
    m.functions["chunk"] = [](auto args) -> Value {
        if (args.size() < 2 || args[0].type() != Value::ARRAY)
            throw std::runtime_error("array::chunk() requires array and size");
        auto& src = *args[0].as_array();
        int chunk_size = args[1].to_int();
        if (chunk_size <= 0) throw std::runtime_error("array::chunk() size must be positive");
        auto result = std::make_shared<std::vector<Value>>();
        for (size_t i = 0; i < src.size(); i += (size_t)chunk_size) {
            size_t end = (std::min)(i + (size_t)chunk_size, src.size());
            auto chunk = std::make_shared<std::vector<Value>>(src.begin() + i, src.begin() + end);
            result->push_back(Value(chunk));
        }
        return Value(result);
    };

    // zip($arr1, $arr2 [, $arr3...]) → [[a1,b1],[a2,b2],...]
    m.functions["zip"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("array::zip() requires at least 2 arrays");
        size_t min_len = ~(size_t)0;
        for (auto& a : args) {
            if (a.type() != Value::ARRAY)
                throw std::runtime_error("array::zip() all arguments must be arrays");
            size_t sz = a.as_array()->size();
            if (sz < min_len) min_len = sz;
        }
        if (min_len == ~(size_t)0) min_len = 0;
        auto result = std::make_shared<std::vector<Value>>();
        for (size_t i = 0; i < min_len; ++i) {
            auto tuple = std::make_shared<std::vector<Value>>();
            for (auto& a : args)
                tuple->push_back((*a.as_array())[i]);
            result->push_back(Value(tuple));
        }
        return Value(result);
    };

    return m;
}

// ── Export ────────────────────────────────────────────────────────────────────

// ── runtime:: ────────────────────────────────────────────────────────────────

#ifdef _WIN32
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

static Module make_runtime_module(Interpreter* interp) {
    Module mod;
    mod.name = "runtime";

    // runtime::stats() → assoc array
    mod.functions["stats"] = [interp](std::vector<Value> args) -> Value {
        long uptime  = interp->get_uptime_sec();
        int  routes  = interp->get_route_count();
        int  reqs    = interp->get_request_count();

        double working_mb = 0.0;
        double private_mb = 0.0;

#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            working_mb = pmc.WorkingSetSize / (1024.0 * 1024.0);
            private_mb = pmc.PrivateUsage  / (1024.0 * 1024.0);
        }
#else
        // Linux: /proc/self/status
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("VmRSS:") == 0) {
                long kb = 0; sscanf(line.c_str(), "VmRSS: %ld", &kb);
                working_mb = kb / 1024.0;
            }
            if (line.find("VmSize:") == 0) {
                long kb = 0; sscanf(line.c_str(), "VmSize: %ld", &kb);
                private_mb = kb / 1024.0;
            }
        }
#endif

        auto arr = std::make_shared<std::vector<Value>>();
        auto push = [&](const std::string& k, Value v) {
            arr->push_back(Value(std::string("__assoc__")));
            // reuse flat key-val push
        };
        // assoc array: ["__assoc__", k, v, k, v, ...]
        arr->push_back(Value(std::string("__assoc__")));
        arr->push_back(Value(std::string("uptime_sec")));   arr->push_back(Value((int)uptime));
        arr->push_back(Value(std::string("request_count"))); arr->push_back(Value(reqs));
        arr->push_back(Value(std::string("route_count")));  arr->push_back(Value(routes));
        arr->push_back(Value(std::string("working_mb")));   arr->push_back(Value(working_mb));
        arr->push_back(Value(std::string("private_mb")));   arr->push_back(Value(private_mb));
        return Value(arr);
    };

    // runtime::gc() → no-op şimdilik, ileride cycle sweep buraya
    mod.functions["gc"] = [](std::vector<Value>) -> Value {
        return Value(std::string("ok"));
    };

    return mod;
}

std::map<std::string, Module> make_extra_stdlib(Interpreter* interp) {
    std::map<std::string, Module> mods;
    auto add = [&](Module mod) { mods[mod.name] = std::move(mod); };
    add(make_auth());
    add(make_validator());
    add(make_array_module(interp));
    add(make_runtime_module(interp));
    add(make_template_module(interp));
    return mods;
}

} // namespace look

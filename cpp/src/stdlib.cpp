#include "look/stdlib.h"
#include "look/logger.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <iomanip>
#include <future>
#include <chrono>

namespace look {

static void check_args(const std::string& fn, size_t got, size_t expected) {
    if (got != expected)
        throw std::runtime_error(fn + "() expects " + std::to_string(expected) +
                                 " argument(s), got " + std::to_string(got));
}
static void check_args_min(const std::string& fn, size_t got, size_t min) {
    if (got < min)
        throw std::runtime_error(fn + "() expects at least " + std::to_string(min) +
                                 " argument(s), got " + std::to_string(got));
}

// â"€â"€ math module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
static Module make_math() {
    Module m;
    m.name = "math";

    m.functions["sqrt"] = [](auto args) {
        check_args("math::sqrt", args.size(), 1);
        return Value(std::sqrt(args[0].to_float()));
    };
    m.functions["pow"] = [](auto args) {
        check_args("math::pow", args.size(), 2);
        return Value(std::pow(args[0].to_float(), args[1].to_float()));
    };
    m.functions["abs"] = [](auto args) {
        check_args("math::abs", args.size(), 1);
        if (args[0].type() == Value::FLOAT) return Value(std::abs(args[0].as_float()));
        return Value(std::abs(args[0].to_int()));
    };
    m.functions["floor"] = [](auto args) {
        check_args("math::floor", args.size(), 1);
        return Value((int)std::floor(args[0].to_float()));
    };
    m.functions["ceil"] = [](auto args) {
        check_args("math::ceil", args.size(), 1);
        return Value((int)std::ceil(args[0].to_float()));
    };
    m.functions["round"] = [](auto args) {
        check_args("math::round", args.size(), 1);
        return Value((int)std::round(args[0].to_float()));
    };
    m.functions["max"] = [](auto args) -> Value {
        if (args.size() == 1 && args[0].type() == Value::ARRAY) {
            auto& arr = *args[0].as_array();
            if (arr.empty()) return Value();
            Value best = arr[0];
            for (size_t i = 1; i < arr.size(); ++i)
                if (arr[i] >= best) best = arr[i];
            return best;
        }
        if (args.size() < 2) throw std::runtime_error("math::max() expects 2+ arguments or 1 array");
        Value best = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] >= best) best = args[i];
        return best;
    };
    m.functions["min"] = [](auto args) -> Value {
        if (args.size() == 1 && args[0].type() == Value::ARRAY) {
            auto& arr = *args[0].as_array();
            if (arr.empty()) return Value();
            Value best = arr[0];
            for (size_t i = 1; i < arr.size(); ++i)
                if (arr[i] <= best) best = arr[i];
            return best;
        }
        if (args.size() < 2) throw std::runtime_error("math::min() expects 2+ arguments or 1 array");
        Value best = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] <= best) best = args[i];
        return best;
    };
    m.functions["pi"] = [](auto args) {
        return Value(3.14159265358979323846);
    };
    m.functions["random"] = [](auto args) -> Value {
        if (args.size() == 2) {
            int lo = args[0].to_int(), hi = args[1].to_int();
            return Value(lo + std::rand() % (hi - lo + 1));
        }
        return Value((double)std::rand() / RAND_MAX);
    };
    m.functions["log"] = [](auto args) {
        check_args("math::log", args.size(), 1);
        return Value(std::log(args[0].to_float()));
    };
    m.functions["sin"] = [](auto args) {
        check_args("math::sin", args.size(), 1);
        return Value(std::sin(args[0].to_float()));
    };
    m.functions["cos"] = [](auto args) {
        check_args("math::cos", args.size(), 1);
        return Value(std::cos(args[0].to_float()));
    };
    m.functions["tan"] = [](auto args) {
        check_args("math::tan", args.size(), 1);
        return Value(std::tan(args[0].to_float()));
    };

    return m;
}

// â"€â"€ string module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
static Module make_string() {
    Module m;
    m.name = "string";

    m.functions["len"] = [](auto args) {
        check_args("string::len", args.size(), 1);
        return Value((int)args[0].to_string().size());
    };
    m.functions["upper"] = [](auto args) {
        check_args("string::upper", args.size(), 1);
        std::string s = args[0].to_string();
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return Value(s);
    };
    m.functions["lower"] = [](auto args) {
        check_args("string::lower", args.size(), 1);
        std::string s = args[0].to_string();
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return Value(s);
    };
    m.functions["trim"] = [](auto args) {
        check_args("string::trim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        return Value(start == std::string::npos ? "" : s.substr(start, end - start + 1));
    };
    m.functions["ltrim"] = [](auto args) {
        check_args("string::ltrim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t start = s.find_first_not_of(" \t\r\n");
        return Value(start == std::string::npos ? "" : s.substr(start));
    };
    m.functions["rtrim"] = [](auto args) {
        check_args("string::rtrim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t end = s.find_last_not_of(" \t\r\n");
        return Value(end == std::string::npos ? "" : s.substr(0, end + 1));
    };
    m.functions["replace"] = [](auto args) {
        check_args("string::replace", args.size(), 3);
        std::string s     = args[0].to_string();
        std::string from  = args[1].to_string();
        std::string to    = args[2].to_string();
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return Value(s);
    };
    m.functions["contains"] = [](auto args) {
        check_args("string::contains", args.size(), 2);
        return Value(args[0].to_string().find(args[1].to_string()) != std::string::npos);
    };
    m.functions["starts_with"] = [](auto args) {
        check_args("string::starts_with", args.size(), 2);
        std::string s = args[0].to_string(), prefix = args[1].to_string();
        return Value(s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix);
    };
    m.functions["ends_with"] = [](auto args) {
        check_args("string::ends_with", args.size(), 2);
        std::string s = args[0].to_string(), suffix = args[1].to_string();
        return Value(s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix);
    };
    m.functions["substr"] = [](auto args) {
        check_args_min("string::substr", args.size(), 2);
        std::string s = args[0].to_string();
        int start = args[1].to_int();
        if (start < 0) start = std::max(0, (int)s.size() + start);
        if (start >= (int)s.size()) return Value(std::string(""));
        if (args.size() == 3) {
            int len = args[2].to_int();
            return Value(s.substr(start, len));
        }
        return Value(s.substr(start));
    };
    m.functions["repeat"] = [](auto args) {
        check_args("string::repeat", args.size(), 2);
        std::string s = args[0].to_string();
        int n = args[1].to_int();
        std::string result;
        for (int i = 0; i < n; i++) result += s;
        return Value(result);
    };
    m.functions["reverse"] = [](auto args) {
        check_args("string::reverse", args.size(), 1);
        std::string s = args[0].to_string();
        std::reverse(s.begin(), s.end());
        return Value(s);
    };
    m.functions["index_of"] = [](auto args) {
        check_args("string::index_of", args.size(), 2);
        std::string s = args[0].to_string(), needle = args[1].to_string();
        size_t pos = s.find(needle);
        return Value(pos == std::string::npos ? -1 : (int)pos);
    };
    // string::split("a,b,c", ",") → ["a","b","c"]
    m.functions["split"] = [](auto args) -> Value {
        check_args_min("string::split", args.size(), 2);
        std::string s   = args[0].to_string();
        std::string sep = args[1].to_string();
        int limit       = args.size() >= 3 ? args[2].to_int() : -1;
        auto arr = std::make_shared<std::vector<Value>>();
        if (sep.empty()) {
            for (char c : s) arr->push_back(Value(std::string(1, c)));
            return Value(arr);
        }
        size_t pos = 0, found;
        int count = 0;
        while ((found = s.find(sep, pos)) != std::string::npos) {
            if (limit > 0 && count >= limit - 1) break;
            arr->push_back(Value(s.substr(pos, found - pos)));
            pos = found + sep.size();
            count++;
        }
        arr->push_back(Value(s.substr(pos)));
        return Value(arr);
    };
    // string::join(["a","b","c"], ",") → "a,b,c"  (alias for built-in join)
    m.functions["join"] = [](auto args) -> Value {
        check_args_min("string::join", args.size(), 1);
        if (args[0].type() != Value::ARRAY) return Value(args[0].to_string());
        std::string sep = args.size() >= 2 ? args[1].to_string() : "";
        std::string result;
        auto& arr = *args[0].as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) result += sep;
            result += arr[i].to_string();
        }
        return Value(result);
    };

    m.functions["pad_left"] = [](auto args) -> Value {
        check_args("string::pad_left", args.size(), 3);
        std::string s   = args[0].to_string();
        int         len = (int)args[1].as_int();
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        while ((int)s.size() < len) s = pad + s;
        if ((int)s.size() > len) s = s.substr(s.size() - len);
        return Value(s);
    };

    m.functions["pad_right"] = [](auto args) -> Value {
        check_args("string::pad_right", args.size(), 3);
        std::string s   = args[0].to_string();
        int         len = (int)args[1].as_int();
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        while ((int)s.size() < len) s = s + pad;
        if ((int)s.size() > len) s = s.substr(0, len);
        return Value(s);
    };

    m.functions["slugify"] = [](auto args) -> Value {
        check_args("string::slugify", args.size(), 1);
        std::string s = args[0].to_string();
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        auto rep = [&](const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        rep("\xc4\xb1", "i"); rep("\xc4\x9f", "g"); rep("\xc3\xbc", "u");
        rep("\xc5\x9f", "s"); rep("\xc3\xb6", "o"); rep("\xc3\xa7", "c");
        rep("\xc4\x9e", "g"); rep("\xc3\x9c", "u"); rep("\xc5\x9e", "s");
        rep("\xc3\x96", "o"); rep("\xc3\x87", "c"); rep("\xc4\xb0", "i");
        std::string result;
        bool last_dash = false;
        for (unsigned char c : s) {
            if (std::isalnum(c)) { result += (char)c; last_dash = false; }
            else if (!last_dash && !result.empty()) { result += '-'; last_dash = true; }
        }
        while (!result.empty() && result.back() == '-') result.pop_back();
        return Value(result);
    };

    m.functions["random"] = [](auto args) -> Value {
        int length = args.size() >= 1 ? args[0].to_int() : 8;
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        static bool seeded = false;
        if (!seeded) { std::srand((unsigned)std::time(nullptr)); seeded = true; }
        std::string result;
        for (int i = 0; i < length; ++i)
            result += chars[std::rand() % (sizeof(chars) - 1)];
        return Value(result);
    };

    // string::format("Merhaba %s, yaşın %d, ortalama %.2f", $ad, $yas, $ort)
    m.functions["format"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("string::format() requires format string");
        std::string fmt = args[0].to_string();
        std::string result;
        size_t arg_idx = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { result += fmt[i]; continue; }
            ++i;
            if (fmt[i] == '%') { result += '%'; continue; }
            bool zero_pad = (fmt[i] == '0');
            int width = 0;
            while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
                width = width * 10 + (fmt[i++] - '0');
            int precision = -1;
            if (i < fmt.size() && fmt[i] == '.') {
                ++i; precision = 0;
                while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
                    precision = precision * 10 + (fmt[i++] - '0');
            }
            if (i >= fmt.size()) break;
            char spec = fmt[i];
            if (arg_idx >= args.size()) { result += '%'; result += spec; continue; }
            Value& v = args[arg_idx++];
            std::ostringstream oss;
            if (zero_pad && width > 0) { oss << std::setfill('0') << std::setw(width); }
            else if (width > 0) { oss << std::setw(width); }
            if (spec == 's') {
                oss << v.to_string();
            } else if (spec == 'd') {
                oss << v.to_int();
            } else if (spec == 'f') {
                if (precision >= 0) oss << std::fixed << std::setprecision(precision);
                oss << v.to_float();
            } else if (spec == 'x') {
                oss << std::hex << v.to_int();
            } else if (spec == 'X') {
                oss << std::uppercase << std::hex << v.to_int();
            } else if (spec == 'o') {
                oss << std::oct << v.to_int();
            } else {
                oss << '%' << spec; --arg_idx;
            }
            result += oss.str();
        }
        return Value(result);
    };

    // ReDoS koruma yardımcısı: regex işlemini ayrı thread'de çalıştır, 250ms timeout uygula
    static auto regex_with_timeout = [](auto fn) -> decltype(fn()) {
        auto fut = std::async(std::launch::async, fn);
        if (fut.wait_for(std::chrono::milliseconds(250)) == std::future_status::timeout)
            throw std::runtime_error("string::regex: execution timeout (ReDoS koruması — pattern çok karmaşık)");
        return fut.get();
    };

    // string::regex_match($str, $pattern) → bool
    m.functions["regex_match"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match() requires string and pattern");
        std::string pat = args[1].to_string();
        std::string str = args[0].to_string();
        if (pat.size() > 2048) throw std::runtime_error("string::regex_match(): pattern too long (max 2048)");
        if (str.size() > 65536) throw std::runtime_error("string::regex_match(): input too long (max 65536)");
        try {
            std::regex re(pat);
            return Value(regex_with_timeout([str, re]{ return std::regex_search(str, re); }));
        } catch (const std::runtime_error&) { throw; }
          catch (...) { return Value(false); }
    };

    // string::regex_replace($str, $pattern, $replacement) → string
    m.functions["regex_replace"] = [](auto args) -> Value {
        if (args.size() < 3) throw std::runtime_error("string::regex_replace() requires string, pattern, replacement");
        std::string pat = args[1].to_string();
        std::string str = args[0].to_string();
        std::string rep = args[2].to_string();
        if (pat.size() > 2048) throw std::runtime_error("string::regex_replace(): pattern too long (max 2048)");
        if (str.size() > 65536) throw std::runtime_error("string::regex_replace(): input too long (max 65536)");
        try {
            std::regex re(pat);
            return Value(regex_with_timeout([str, re, rep]{ return std::regex_replace(str, re, rep); }));
        } catch (const std::runtime_error&) { throw; }
          catch (...) { return Value(args[0].to_string()); }
    };

    // string::regex_match_all($str, $pattern) → [[tam_eşleşme, grup1, ...], ...]
    m.functions["regex_match_all"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match_all() requires string and pattern");
        auto result = std::make_shared<std::vector<Value>>();
        try {
            std::string s = args[0].to_string();
            std::string pat = args[1].to_string();
            if (pat.size() > 2048) throw std::runtime_error("string::regex_match_all(): pattern too long (max 2048)");
            if (s.size() > 65536) throw std::runtime_error("string::regex_match_all(): input too long (max 65536)");
            std::regex re(pat);
            auto matches = regex_with_timeout([s, re]{
                std::vector<std::vector<std::string>> out;
                auto it = std::sregex_iterator(s.begin(), s.end(), re);
                for (auto end_it = std::sregex_iterator(); it != end_it; ++it) {
                    std::vector<std::string> m;
                    for (size_t i = 0; i < it->size(); ++i) m.push_back((*it)[i].str());
                    out.push_back(std::move(m));
                }
                return out;
            });
            for (auto& m : matches) {
                auto match = std::make_shared<std::vector<Value>>();
                for (auto& s2 : m) match->push_back(Value(s2));
                result->push_back(Value(match));
            }
        } catch (const std::runtime_error&) { throw; }
          catch (...) {}
        return Value(result);
    };

    return m;
}

// ── type module ───────────────────────────────────────────────────────────────
static Module make_type() {
    Module m;
    m.name = "type";

    m.functions["of"] = [](auto args) -> Value {
        check_args("type::of", args.size(), 1);
        switch (args[0].type()) {
            case Value::INT:      return Value(std::string("int"));
            case Value::FLOAT:    return Value(std::string("float"));
            case Value::STRING:   return Value(std::string("string"));
            case Value::BOOL:     return Value(std::string("bool"));
            case Value::FUNCTION: return Value(std::string("function"));
            case Value::ARRAY:    return Value(std::string("array"));
            case Value::NONE:     return Value(std::string("null"));
        }
        return Value(std::string("unknown"));
    };
    m.functions["is_null"]     = [](auto a) { check_args("type::is_null",     a.size(),1); return Value(a[0].type()==Value::NONE); };
    m.functions["is_string"]   = [](auto a) { check_args("type::is_string",   a.size(),1); return Value(a[0].type()==Value::STRING); };
    m.functions["is_int"]      = [](auto a) { check_args("type::is_int",      a.size(),1); return Value(a[0].type()==Value::INT); };
    m.functions["is_float"]    = [](auto a) { check_args("type::is_float",    a.size(),1); return Value(a[0].type()==Value::FLOAT); };
    m.functions["is_bool"]     = [](auto a) { check_args("type::is_bool",     a.size(),1); return Value(a[0].type()==Value::BOOL); };
    m.functions["is_array"]    = [](auto a) { check_args("type::is_array",    a.size(),1); return Value(a[0].type()==Value::ARRAY); };
    m.functions["is_function"] = [](auto a) { check_args("type::is_function", a.size(),1); return Value(a[0].type()==Value::FUNCTION); };
    m.functions["to_int"]    = [](auto a) { check_args("type::to_int",    a.size(),1); return Value(a[0].to_int()); };
    m.functions["to_float"]  = [](auto a) { check_args("type::to_float",  a.size(),1); return Value(a[0].to_float()); };
    m.functions["to_string"] = [](auto a) { check_args("type::to_string", a.size(),1); return Value(a[0].to_string()); };
    m.functions["to_bool"]   = [](auto a) { check_args("type::to_bool",   a.size(),1); return Value(a[0].is_truthy()); };

    return m;
}

// â"€â"€ log module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€

static Module make_html() {
    Module m;
    m.name = "html";

    m.functions["escape"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        out.reserve(s.size() * 2);
        for (unsigned char c : s) {
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                default:   out += (char)c;
            }
        }
        return Value(out);
    };

    m.functions["attr"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        for (unsigned char c : s) {
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                default:   out += (char)c;
            }
        }
        return Value(out);
    };

    m.functions["strip"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        bool in_tag = false;
        for (char c : s) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) out += c;
        }
        return Value(out);
    };

    return m;
}

static Module make_log() {
    Module m;
    m.name = "log";

    auto do_log = [](LogLevel level, const std::string& cat, auto args) {
        if (args.empty()) return Value();
        std::string msg;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) msg += " ";
            msg += args[i].to_string();
        }
        Logger::instance().log(level, cat, msg);
        return Value();
    };

    m.functions["info"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_INFO, "APP", args);
    };
    m.functions["error"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_ERROR, "APP", args);
    };
    m.functions["warn"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_WARN, "APP", args);
    };
    m.functions["debug"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_DEBUG, "APP", args);
    };
    m.functions["query"] = [](auto args) -> Value {
        if (args.size() < 2) return Value();
        Logger::instance().log_query(args[0].to_string(), args[1].to_int());
        return Value();
    };
    m.functions["memory"] = [do_log](auto args) -> Value {
        // Windows'ta working set size
        // Basit: sadece bir mesaj log'la
        return do_log(LogLevel::LOG_INFO, "MEM", args);
    };
    m.functions["configure"] = [](auto args) -> Value {
        // log::configure("logs", true, "debug")
        std::string dir     = args.size() > 0 ? args[0].to_string() : "logs";
        bool verbose        = args.size() > 1 ? args[1].is_truthy() : false;
        std::string level_s = args.size() > 2 ? args[2].to_string() : "info";
        LogLevel level = LogLevel::LOG_INFO;
        if (level_s == "debug") level = LogLevel::LOG_DEBUG;
        if (level_s == "warn")  level = LogLevel::LOG_WARN;
        if (level_s == "error") level = LogLevel::LOG_ERROR;
        Logger::instance().configure(dir, verbose, level);
        return Value();
    };

    return m;
}

// â"€â"€ Registry â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
// Forward declarations — defined in their respective .cpp files
Module make_http_module();
Module make_cache_module();
Module make_queue_module();
Module make_jobs_module();
Module make_mail_module();

std::map<std::string, Module> make_stdlib() {
    std::srand((unsigned)std::time(nullptr));
    std::map<std::string, Module> stdlib;
    auto add = [&](Module mod) { stdlib[mod.name] = std::move(mod); };
    add(make_math());
    add(make_string());
    add(make_type());
    add(make_html());
    add(make_log());
    add(make_file_module());
    add(make_date_module());
    add(make_http_module());
    add(make_cache_module());
    add(make_queue_module());
    add(make_jobs_module());
    add(make_mail_module());
    return stdlib;
}

} // namespace look



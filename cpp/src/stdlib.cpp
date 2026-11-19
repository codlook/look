#include "look/mem_budget.h"   // mem_reserve — budget before a large allocation
#include "look/num_ops.h"   // single definition of abs/floor/ceil/round
#include "look/stdlib.h"
#include "look/value_path.h"   // value_list_only — a list-specific operation on a dictionary is an error (single definition)
#include "look/html_escape.h"
#include "look/charset.h"
#include "look/parallel_runtime.h"
#include "look/logger.h"
#include "look/struct_types.h"
#include <random>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <iomanip>
#include <optional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <functional>
#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <fstream>
#  include <pthread.h>
#  include <sys/resource.h>
#endif
#include <memory>

namespace look {

// CSPRNG byte generation — for paths inviting token/key generation
// like string::random. std::rand() is NOT CRYPTOGRAPHIC (second-seed + LCG = predictable
// → if an API key/reset token is generated it is an auth bypass). Fail-closed:
// if there is no secure source, throw an error instead of emitting weak output. (Same discipline as web.cpp
// random_hex and the session ID.)
static void secure_random_fill(uint8_t* buf, size_t n) {
    if (n == 0) return;
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("string::random: CSPRNG unavailable (BCryptGenRandom)");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(buf), (std::streamsize)n);
    if (!urandom || (size_t)urandom.gcount() != n)
        throw std::runtime_error("string::random: CSPRNG unavailable (/dev/urandom)");
#endif
}

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
        double x = args[0].to_float();
        // Negative → a clear error instead of a silent NaN. If NaN later got into JSON
        // it would produce invalid JSON (NaN is forbidden).
        if (x < 0) throw std::runtime_error("math::sqrt: square root of a negative number is undefined");
        return Value(std::sqrt(x));
    };
    m.functions["pow"] = [](auto args) {
        check_args("math::pow", args.size(), 2);
        return Value(std::pow(args[0].to_float(), args[1].to_float()));
    };
    m.functions["abs"] = [](auto args) {
        check_args("math::abs", args.size(), 1);
        return num_abs(args[0]);      // look/num_ops.h: INT64_MIN → decimal (previously UB)
    };
    // floor/ceil/round: a result that fits is an integer, one that does not is a decimal (previously the (int64_t) conversion was UB:
    // floor(1e300) → INT64_MIN). An integer input is returned as is (precision above 2^53 is preserved).
    m.functions["floor"] = [](auto args) {
        check_args("math::floor", args.size(), 1);
        return num_floor(args[0]);
    };
    m.functions["ceil"] = [](auto args) {
        check_args("math::ceil", args.size(), 1);
        return num_ceil(args[0]);
    };
    m.functions["round"] = [](auto args) {
        check_args("math::round", args.size(), 1);
        return num_round(args[0]);
    };
    m.functions["max"] = [](auto args) -> Value {
        for (auto& x : args) look::value_list_only(x, "math::max()");   // dictionary: error (value_path.h)
        if (args.size() < 2 && !(args.size() == 1 && args[0].type() == Value::ARRAY))
            throw std::runtime_error("math::max() expects 2+ arguments or 1 array");
        return num_extreme(args, true);   // single definition with core max() (look/num_ops.h)
    };
    m.functions["min"] = [](auto args) -> Value {
        for (auto& x : args) look::value_list_only(x, "math::min()");
        if (args.size() < 2 && !(args.size() == 1 && args[0].type() == Value::ARRAY))
            throw std::runtime_error("math::min() expects 2+ arguments or 1 array");
        return num_extreme(args, false);
    };
    m.functions["pi"] = [](auto args) {
        return Value(3.14159265358979323846);
    };
    // math::random — EACH THREAD has its own generator and each is seeded separately from the operating system's secure
    // source. Previously a single C `rand()` was used and with every new copy of the standard library it was
    // re-seeded with `srand(time)`: threads created in the same second produced the SAME sequence
    // (measured: 14 distinct numbers across 16 concurrent requests), and the sequence was
    // predictable from the second. Also `rand() % n` could not exceed 32767 on Windows.
    // NOTE: this generator is NOT for security purposes (use crypto::random_string for codes/keys).
    m.functions["random"] = [](auto args) -> Value {
        // fiber-ok: random generator; no suspension between write and read
        static thread_local std::mt19937_64 engine = []() {
            uint8_t seed_bytes[32];
            secure_random_fill(seed_bytes, sizeof(seed_bytes));
            uint32_t words[8];
            std::memcpy(words, seed_bytes, sizeof(words));
            std::seed_seq seq(words, words + 8);
            return std::mt19937_64(seq);
        }();
        if (args.size() == 2) {
            int64_t lo = args[0].to_int(), hi = args[1].to_int();
            if (hi < lo) throw std::runtime_error("math::random: hi < lo (invalid range)");
            return Value((int64_t)std::uniform_int_distribution<int64_t>(lo, hi)(engine));
        }
        return Value(std::generate_canonical<double, 53>(engine));
    };
    m.functions["log"] = [](auto args) {
        check_args("math::log", args.size(), 1);
        double x = args[0].to_float();
        // x ≤ 0 → log(0)=-Inf, log(negative)=NaN. Give a clear error like sqrt — otherwise
        // NaN/Inf later get into JSON and produce invalid JSON.
        if (x <= 0) throw std::runtime_error("math::log: logarithm of a non-positive number is undefined");
        return Value(std::log(x));
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
// ── UTF-8 helpers ───────────────────────────────────────────────────────────
// LOOK strings are UTF-8. Byte-based len/upper/substr/reverse split multi-byte
// characters (ç,ğ,İ,ı,ö,ş,ü…) and produced invalid UTF-8.
// These helpers are codepoint-aware; critical for the Turkish market.
static std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80)             { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
        else                      { cps.push_back(0xFFFD); i++; continue; }
        if (i + (size_t)len > n)  { cps.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { cps.push_back(0xFFFD); i++; continue; }
        cps.push_back(cp);
        i += (size_t)len;
    }
    return cps;
}
static void utf8_encode_cp(uint32_t cp, std::string& out) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}
static std::string utf8_encode(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) utf8_encode_cp(cp, out);
    return out;
}
// Codepoint upper/lower case — locale-INDEPENDENT (like classic languages): i↔I.
// There is NO Turkish-specific i↔İ / ı↔I locale rule (the language is global). Turkish letters
// still map to their standard Unicode counterparts: ç↔Ç, ğ↔Ğ, ö↔Ö, ş↔Ş, ü↔Ü, ı→I, İ→i.
// COVERAGE: it used to have only ASCII + Latin-1 + 3 Turkish code points (ı ğ ş).
// That is, the RULE was global but the TABLE was specific to Turkey: Cyrillic, Greek and the
// remaining ~125 characters of Latin Ext-A passed through silently unconverted —
//   upper("привет") = "привет"  (no change at all)
//   upper("ελλάδα") = "ελλάδα"
//   upper("łódź")   = "łÓDź"    (HALF conversion: o,d converted; ł,ź did not)
// No error, no warning: silently wrong/mixed text. The ranges below were added.
//
// ATTENTION: Latin Ext-A's general even/odd rule gives ı(0x131) -> İ(0x130), i.e. it
// brings back TURKISH locale behavior. So the Turkish exceptions are handled
// BEFORE the rule (the language must stay global: ı → I, İ → i).
static uint32_t cp_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;   // ASCII: i → I
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32; // Latin-1 (à→À, ç→Ç, ö→Ö, ü→Ü…)
    switch (c) {
        case 0x131: return 'I';   // ı → I (standard Unicode — it would be İ in Turkish)
        case 0x11F: return 0x11E; // ğ → Ğ
        case 0x15F: return 0x15E; // ş → Ş
        case 0x17F: return 'S';   // ſ (long s) → S
        case 0x3C2: return 0x3A3; // ς (son sigma) → Σ
        case 0x3AC: return 0x386; // ά → Ά
        case 0x3CC: return 0x38C; // ό → Ό
    }
    // Latin Extended-A (0x100–0x17F): mostly paired.
    // 0x100–0x137 and 0x14A–0x177: EVEN = upper, ODD = lower
    // 0x139–0x148 and 0x179–0x17E: ODD = upper, EVEN = lower
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c % 2) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c % 2 == 0) ? c - 1 : c;
    // Greek: α–ρ, σ–ϋ  (0x3B1–0x3C1, 0x3C3–0x3CB)
    if ((c >= 0x3B1 && c <= 0x3C1) || (c >= 0x3C3 && c <= 0x3CB)) return c - 0x20;
    if (c >= 0x3AD && c <= 0x3AF) return c - 0x25; // έ ή ί → Έ Ή Ί
    if (c >= 0x3CD && c <= 0x3CE) return c - 0x3F; // ύ ώ → Ύ Ώ
    // Cyrillic: а–я (0x430–0x44F), ѐ–џ (0x450–0x45F)
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}
static uint32_t cp_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;   // ASCII: I → i
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32; // Latin-1 (À→à, Ç→ç, Ö→ö, Ü→ü…)
    switch (c) {
        case 0x130: return 'i';   // İ → i (standard Unicode — it would be ı in Turkish)
        case 0x11E: return 0x11F; // Ğ → ğ
        case 0x15E: return 0x15F; // Ş → ş
        case 0x178: return 0xFF;  // Ÿ → ÿ
        case 0x386: return 0x3AC; // Ά → ά
        case 0x38C: return 0x3CC; // Ό → ό
    }
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c % 2 == 0) ? c + 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c % 2) ? c + 1 : c;
    if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 0x20;
    if (c >= 0x388 && c <= 0x38A) return c + 0x25; // Έ Ή Ί → έ ή ί
    if (c >= 0x38E && c <= 0x38F) return c + 0x3F; // Ύ Ώ → ύ ώ
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

// Padding — Go semantics (fmt "%Ns"): the target width is a MINIMUM and COUNTS CODE POINTS.
// If the text is already longer it is returned AS IS; the padding function never truncates.
//
// OLD BUG 1 (silent data loss): long text was truncated —
//   pad_left("abcdef", 3, "0") = "def"   (PHP str_pad / Python ljust do not truncate)
// OLD BUG 2 (corrupt output): the truncation was BYTE based and split a multi-byte character
//   in the middle — pad_left("şğü", 3, "x") = 0x9F 0xC3 0xBC, INVALID UTF-8 starting
//   with an orphan continuation byte. This corrupt text went as is into JSON, the database and the
//   HTTP response.
// OLD BUG 3 (inconsistency): the width counted BYTES, whereas len/substr/upper/
//   reverse are all code-point aware — pad_left("ş", 3, "x") = "xş",
//   visible length 2 (expected 3).
static std::string pad_to(const std::string& s, int64_t len,
                          const std::string& pad, bool left) {
    int64_t cur = (int64_t)utf8_decode(s).size();
    if (cur >= len) return s;                 // MINIMUM width: NO truncation
    auto padcps = utf8_decode(pad);
    if (padcps.empty()) return s;             // empty padding -> do not enter an infinite loop
    std::string fill;
    int64_t need = len - cur;
    while (need > 0)
        for (uint32_t cp : padcps) {
            if (need <= 0) break;
            utf8_encode_cp(cp, fill);
            --need;
        }
    return left ? fill + s : s + fill;
}

static Module make_string() {
    Module m;
    m.name = "string";

    // Character (codepoint) count — not bytes. len("İstanbul") = 8.
    m.functions["len"] = [](auto args) {
        check_args("string::len", args.size(), 1);
        return Value((int64_t)utf8_decode(args[0].to_string()).size());
    };
    // Convert a single-byte charset (iso-8859-9, windows-1254, latin1/1252) to UTF-8.
    // Unknown charset → the input is returned as is. The HTTP client automatically uses the same function
    // for the Content-Type charset.
    m.functions["decode"] = [](auto args) -> Value {
        check_args("string::decode", args.size(), 2);
        std::string in = args[0].to_string();
        std::string cs = args[1].to_string();
        std::string out;
        if (look::charset_to_utf8(in, cs, out)) return Value(out);
        return Value(in);
    };
    // Uppercase — locale-independent (i→I). Multi-byte characters are not corrupted.
    m.functions["upper"] = [](auto args) {
        check_args("string::upper", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        for (auto& c : cps) c = cp_upper(c);
        return Value(utf8_encode(cps));
    };
    // Lowercase — locale-independent (I→i).
    m.functions["lower"] = [](auto args) {
        check_args("string::lower", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        for (auto& c : cps) c = cp_lower(c);
        return Value(utf8_encode(cps));
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
        // EMPTY search string — Go semantics (strings.Replace(s, "", new, -1)):
        // matches at the START of the text and AFTER each UTF-8 sequence; k+1 insertions
        // for k code points. "abc" + "X" -> "XaXbXcX". Bounded and defined.
        // OLD BUG: the loop below spun FOREVER with an empty 'from' —
        // s.find("", pos) always returns pos, 'to' is inserted at each turn,
        // the string keeps growing. Infinite loop + UNBOUNDED MEMORY. On the web, in a
        // string::replace($text, $userInput, $x) call, if the user sends an empty
        // string the worker locked up: a DoS with a single request.
        if (from.empty()) {
            auto cps = utf8_decode(s);
            std::string out;
            out.reserve(s.size() + to.size() * (cps.size() + 1));
            out += to;
            for (uint32_t cp : cps) { utf8_encode_cp(cp, out); out += to; }
            return Value(out);
        }
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
    // Codepoint-based: substr("çğü",0,2) = "çğ" (not half a character).
    m.functions["substr"] = [](auto args) {
        check_args_min("string::substr", args.size(), 2);
        std::string src = args[0].to_string();
        // ASCII fast-path: for pure-ASCII input, byte offset == codepoint offset, so we
        // can slice bytes directly — no utf8_decode (which allocated a vector<uint32_t> of
        // the WHOLE string on every call, making substr O(input) instead of O(slice); a
        // 1000-char haystack was decoded 2M times in the string_slice benchmark). Detecting
        // ASCII is a cheap byte scan; the multi-byte path below is unchanged for correctness.
        bool ascii = true;
        for (unsigned char ch : src) { if (ch >= 0x80) { ascii = false; break; } }
        if (ascii) {
            int64_t n = (int64_t)src.size();
            int64_t start = args[1].to_int();
            if (start < 0) start = std::max((int64_t)0, n + start);
            if (start >= n) return Value(std::string(""));
            int64_t count = (args.size() == 3) ? args[2].to_int() : (n - start);
            if (count < 0) count = 0;
            if (count > n - start) count = n - start;
            return Value(src.substr((size_t)start, (size_t)count));
        }
        auto cps = utf8_decode(src);
        int64_t n = (int64_t)cps.size();
        int64_t start = args[1].to_int();
        if (start < 0) start = std::max((int64_t)0, n + start);
        if (start >= n) return Value(std::string(""));
        int64_t count = (args.size() == 3) ? args[2].to_int() : (n - start);
        if (count < 0) count = 0;
        // Prevent OVERFLOW: count is clamped to the remaining length (n-start) FIRST — otherwise
        // start+count overflows int64 and produces a negative end (vector(first>last) → length_error,
        // not a memory-safety issue but UB iterator arithmetic). Clamped, it is correct: return the remainder.
        if (count > n - start) count = n - start;
        int64_t end = start + count;   // no longer overflows (count ≤ n-start)
        std::vector<uint32_t> sub(cps.begin() + start, cps.begin() + end);
        return Value(utf8_encode(sub));
    };
    m.functions["repeat"] = [](auto args) {
        check_args("string::repeat", args.size(), 2);
        std::string s = args[0].to_string();
        int n = args[1].to_int();
        if (n < 0) throw std::runtime_error("string::repeat: repeat count cannot be negative");
        static constexpr int64_t MAX_REPEAT = 10 * 1024 * 1024; // 10 MB
        if ((int64_t)n * (int64_t)s.size() > MAX_REPEAT)
            throw std::runtime_error("string::repeat: result exceeds the 10 MB limit");
        look::mem_reserve((int64_t)n * (int64_t)s.size(), "string::repeat()");   // budget before allocating
        std::string result;
        result.reserve((size_t)n * s.size());
        for (int i = 0; i < n; i++) result += s;
        return Value(result);
    };
    // Codepoint-based: reverse("abç") = "çba" (characters reversed, not bytes).
    m.functions["reverse"] = [](auto args) {
        check_args("string::reverse", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        std::reverse(cps.begin(), cps.end());
        return Value(utf8_encode(cps));
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
        auto arr = std::make_shared<ValueArray>();
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
        look::value_list_only(args[0], "string::join()");
        std::string sep = args.size() >= 2 ? args[1].to_string() : "";
        std::string result;
        auto& arr = *args[0].as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) result += sep;
            result += arr[i].to_string();
        }
        return Value(result);
    };

    // Upper limit on the target length — `pad_left("x", 2000000000, " ")` is an unbounded/O(n²)
    // allocation (every turn `pad + s` is recopied) → hang/OOM DoS. Same discipline as string::repeat
    // (10MB) and string::random (1MB).
    static constexpr int PAD_MAX = 10 * 1024 * 1024;
    m.functions["pad_left"] = [](auto args) -> Value {
        check_args("string::pad_left", args.size(), 3);
        std::string s   = args[0].to_string();
        int64_t     len = args[1].as_int();
        if (len < 0) len = 0;
        if (len > PAD_MAX) throw std::runtime_error("string::pad_left: target length exceeds the 10 MB limit");
        look::mem_reserve(len, "string::pad_left()");
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        return Value(pad_to(s, len, pad, true));
    };

    m.functions["pad_right"] = [](auto args) -> Value {
        check_args("string::pad_right", args.size(), 3);
        std::string s   = args[0].to_string();
        int64_t     len = args[1].as_int();
        if (len < 0) len = 0;
        if (len > PAD_MAX) throw std::runtime_error("string::pad_right: target length exceeds the 10 MB limit");
        look::mem_reserve(len, "string::pad_right()");
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        return Value(pad_to(s, len, pad, false));
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
        int length = args.size() >= 1 ? (int)args[0].to_int() : 8;
        if (length < 0) length = 0;
        static constexpr int MAX_RANDOM = 1 << 20;   // 1 MB upper limit (OOM DoS)
        if (length > MAX_RANDOM) length = MAX_RANDOM;
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::string result((size_t)length, '\0');
        // Rejection sampling: 256 is not divisible by 36 — `byte % 36` produced the first 4 characters of the alphabet
        // (a, b, c, d) ~14% more often than the others. Bytes below 252 (7 × 36) are
        // used, the rest are discarded and a new one is drawn; every character is equally likely.
        static constexpr unsigned ALPHA = sizeof(chars) - 1;          // 36
        static constexpr unsigned LIMIT = (256 / ALPHA) * ALPHA;      // 252
        size_t filled = 0;
        while (filled < (size_t)length) {
            std::vector<uint8_t> buf((size_t)length - filled + 16);
            secure_random_fill(buf.data(), buf.size());   // CSPRNG (fail-closed)
            for (size_t i = 0; i < buf.size() && filled < (size_t)length; ++i)
                if (buf[i] < LIMIT) result[filled++] = chars[buf[i] % ALPHA];
        }
        return Value(result);
    };

    // string::format("Hello %s, your age %d, average %.2f", $name, $age, $avg)
    m.functions["format"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("string::format() requires format string");
        std::string fmt = args[0].to_string();
        std::string result;
        size_t arg_idx = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { result += fmt[i]; continue; }
            ++i;
            if (fmt[i] == '%') { result += '%'; continue; }
            // Width/precision upper limit — a huge width such as `%2000000000d` would
            // allocate hundreds of MB of padding via setw, causing a memory-amplification DoS (and int
            // overflow UB). 1M is more than enough; same discipline as the string::repeat 10MB
            // cap.
            static constexpr int FMT_MAX = 1000000;
            bool zero_pad = (fmt[i] == '0');
            int width = 0;
            while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) {
                width = width * 10 + (fmt[i++] - '0');
                if (width > FMT_MAX) width = FMT_MAX;   // clamp — before overflow
            }
            int precision = -1;
            if (i < fmt.size() && fmt[i] == '.') {
                ++i; precision = 0;
                while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) {
                    precision = precision * 10 + (fmt[i++] - '0');
                    if (precision > FMT_MAX) precision = FMT_MAX;
                }
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

    // ReDoS protection helper: thread::detach + condition_variable (250ms timeout)
    // std::async is not used — its destructor blocks and makes the timeout ineffective.
    static std::atomic<int> g_regex_threads{0};
    static constexpr int REGEX_MAX_THREADS = 8;
    // Compiled-pattern cache: std::regex construction is expensive (NFA build), and the
    // string:: regex builtins used to recompile the pattern on EVERY call — 100k calls =
    // 100k compilations (measured: the bulk of regex cost, not the match). PHP (preg_*)
    // and V8 both cache; LOOK did not. thread_local so no locking (each worker has its own),
    // capped so distinct-pattern floods can't grow it unbounded. std::regex::optimize now
    // pays off: slower compile once, faster match on every reuse.
    // (Routing patterns are already compiled once per route; this covers the string:: API.)
    static auto cached_regex = [](const std::string& pat) -> const std::regex& {
        // fiber-ok: compiled pattern cache; the job takes a COPY of the regex, waiting is via cv (no suspension)
        static thread_local std::unordered_map<std::string, std::regex> cache;
        auto it = cache.find(pat);
        if (it != cache.end()) return it->second;
        if (cache.size() >= 256) cache.clear();
        std::regex re(pat, std::regex::optimize);   // may throw regex_error → caller catches
        return cache.emplace(pat, std::move(re)).first->second;
    };

    // Persistent guard-thread pool. The ReDoS guard used to spawn a std::thread on EVERY
    // regex call (~50µs). Measured, with the compile cache already in place: bypassing the
    // guard dropped a 100k match-all from 6477ms to 189ms — the per-call spawn was ~54% of
    // the remaining cost, not std::regex's match speed. A fixed pool of REGEX_MAX_THREADS
    // workers reuses threads (µs handoff) and preserves the F-01 semantics EXACTLY: a task
    // that runs away (catastrophic backtracking, input-bounded at 64KB so it still ends)
    // holds its worker until it finishes, so at most REGEX_MAX_THREADS run concurrently and
    // the (N+1)th caller is rejected fail-loud by the g_regex_threads cap below — same bound
    // as the old spawn model. Leaked singleton (process-lifetime) — no static-dtor ordering.
    //
    // STACK: std::regex (libstdc++ and MSVC) does matching recursively, the depth grows with the input
    // length. On the default 8 MB stack ~10–20 KB of input overflowed the stack and brought down the WHOLE PROCESS
    // (SIGSEGV) — while the allowed limit is 64 KB. Measured on LOOK's OWN -O3
    // build (a separate test program gives smaller frames and misleads), 64 KB input:
    // at 64 MB 5 of 7 patterns passed, at 96 5, at 128 6, at 256 7 → 256 MB. The need grows with the pattern's
    // nested group depth; 256 MB covers every pattern tried, not an unbounded one.
    // Workers are opened one by one AS NEEDED (a single worker in most processes). 1.0.34 opened all 8
    // at once (≈2 GB virtual): at an `ulimit -v` limit (shared hosting, ~1 GB) some
    // could not be opened and the process HUNG. Now instead of a worker that cannot be opened the job is queued to the open workers;
    // if NO worker can be opened, an understandable error. Workers live for the process lifetime → detach.
    static constexpr size_t REGEX_STACK = size_t(256) << 20;
    struct RegexPool {
        // run: the worker runs it. fail: if no worker is left (could not be opened) ends the job with an error — a job
        // left in the queue without a worker would make the caller wait 250 ms and give a misleading "timeout".
        struct Job { std::function<void()> run, fail; };
        std::queue<Job>                   q;
        std::mutex                        m;
        std::condition_variable           cv;
        int                               started = 0;   // running workers
        int                               pending = 0;   // workers being opened
        int                               idle    = 0;   // workers waiting for a job
        static bool spawn(std::function<void()>* body) {
#ifdef _WIN32
            HANDLE h = CreateThread(nullptr, REGEX_STACK, [](LPVOID p) -> DWORD {
                std::unique_ptr<std::function<void()>> f((std::function<void()>*)p); (*f)(); return 0;
            }, body, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!h) { delete body; return false; }
            CloseHandle(h);
#else
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, REGEX_STACK);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_t t;
            int rc = pthread_create(&t, &at, [](void* p) -> void* {
                std::unique_ptr<std::function<void()>> f((std::function<void()>*)p); (*f)(); return nullptr;
            }, body);
            pthread_attr_destroy(&at);
            if (rc != 0) { delete body; return false; }
#endif
            return true;
        }
        void worker() {
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lk(m);
                    ++idle;
                    cv.wait(lk, [this]{ return !q.empty(); });
                    --idle;
                    job = std::move(q.front()); q.pop();
                }
                job.run();   // runs fn(), fills the caller's State, then decrements the slot
            }
        }
        // In a process with limited virtual memory (ulimit -v / RLIMIT_AS — shared hosting) 256 MB
        // regex workers took the place of the server's connection fibers (8 MB stack): measured,
        // at an "own size + 1 GB" limit when 3 regex workers were opened a connection's fiber could not allocate a stack
        // and the connection dropped. If there is a limit the worker count is as many as fit in 1/8 of the limit (at least 1); otherwise 8.
        // Read at every growth decision (the limit can change while the process runs — prlimit).
        static int worker_cap() {
#ifndef _WIN32
            struct rlimit rl;
            if (getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
                unsigned long long n = (unsigned long long)rl.rlim_cur / 8 / REGEX_STACK;
                return (int)std::max<unsigned long long>(1, std::min<unsigned long long>(n, REGEX_MAX_THREADS));
            }
#endif
            return REGEX_MAX_THREADS;
        }
        // Put the job in the queue; if there is no idle worker (and the limit is not reached) open a new worker. If it cannot be opened and there is no
        // worker/none can be opened, ALL queued jobs finish immediately with fail (when two requests tried
        // to open at the same time and both failed, one stayed in the queue without a worker).
        void submit(Job job) {
            bool grow;
            const int cap = worker_cap();
            {
                std::lock_guard<std::mutex> lk(m);
                q.push(std::move(job));
                grow = idle == 0 && started + pending < cap;
                if (grow) ++pending;
            }
            cv.notify_one();
            if (!grow) return;
            bool ok;
            try { ok = spawn(new std::function<void()>([this]{ worker(); })); } catch (...) { ok = false; }
            std::vector<Job> orphans;
            {
                std::lock_guard<std::mutex> lk(m);
                --pending;
                if (ok) ++started;
                else if (started == 0 && pending == 0)
                    while (!q.empty()) { orphans.push_back(std::move(q.front())); q.pop(); }
            }
            for (auto& o : orphans) o.fail();
        }
    };
    // The pool object is built empty; workers are opened on the first regex (an lk / lk-cgi
    // process that uses no regex starts no thread — startup_threads_test).
    static auto& regex_pool = *new RegexPool();

    static auto regex_with_timeout = [](auto fn) -> decltype(fn()) {
        if (g_regex_threads.load() >= REGEX_MAX_THREADS)
            throw std::runtime_error("string::regex: concurrent regex limit exceeded (max 8)");
        using T = decltype(fn());
        struct State {
            std::mutex              mtx;
            std::condition_variable cv;
            bool                    done       = false;
            bool                    timed_out  = false; // who will decrement the counter
            std::optional<T>        result;
            std::exception_ptr      ex;
        };
        auto state = std::make_shared<State>();

        g_regex_threads++;
        // fail: no regex worker could be opened — end the job with an error, give the slot back (as in run).
        auto fail = [state]() {
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->ex = std::make_exception_ptr(std::runtime_error(
                    "string::regex: could not start a regex thread (256 MB stack; is the process's virtual memory limited?)"));
                state->done = true;
            }
            g_regex_threads--;
            state->cv.notify_one();
        };
        regex_pool.submit({[state, fn = std::move(fn)]() mutable {
            try {
                T r = fn();
                std::lock_guard<std::mutex> lk(state->mtx);
                state->result = std::move(r);
            } catch (...) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->ex = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->done = true;
            }
            // F-01: ALWAYS release the slot when the worker (this thread) actually finishes —
            // NOT on the main-thread timeout. Otherwise the slot frees up but the runaway thread keeps burning fn()
            // (catastrophic regex) → the limit of 8 does not bound LIVE threads
            // (N requests → N burning threads → remote DoS). This line is the single decrement point:
            // main does not decrement so there is no double decrement, and the worker decrements so there is no leak either.
            g_regex_threads--;
            state->cv.notify_one();
        }, fail});

        std::unique_lock<std::mutex> lk(state->mtx);
        if (!state->cv.wait_for(lk, std::chrono::milliseconds(250), [&]{ return state->done; })) {
            // Timeout: end the request quickly BUT do NOT decrement the counter — the runaway thread is still
            // running; the slot is released only when that thread finishes (above) → at the same time
            // there can be at most 8 LIVE regex threads, the 9th is rejected at the cap check (the system slows down
            // but does not die). timed_out is meaningful only during result/notify.
            state->timed_out = true;
            throw std::runtime_error("string::regex: execution timeout (ReDoS protection — pattern too complex)");
        }
        if (state->ex) std::rethrow_exception(state->ex);
        return std::move(*state->result);
    };

    // string::regex_match($str, $pattern) → bool
    m.functions["regex_match"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match() requires string and pattern");
        std::string pat = args[1].to_string();
        std::string str = args[0].to_string();
        if (pat.size() > 2048) throw std::runtime_error("string::regex_match(): pattern too long (max 2048)");
        if (str.size() > 65536) throw std::runtime_error("string::regex_match(): input too long (max 65536)");
        try {
            const std::regex& re = cached_regex(pat);
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
            const std::regex& re = cached_regex(pat);
            return Value(regex_with_timeout([str, re, rep]{ return std::regex_replace(str, re, rep); }));
        } catch (const std::runtime_error&) { throw; }
          catch (...) { return Value(args[0].to_string()); }
    };

    // string::regex_match_all($str, $pattern) → [[full_match, group1, ...], ...]
    m.functions["regex_match_all"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match_all() requires string and pattern");
        auto result = std::make_shared<ValueArray>();
        try {
            std::string s = args[0].to_string();
            std::string pat = args[1].to_string();
            if (pat.size() > 2048) throw std::runtime_error("string::regex_match_all(): pattern too long (max 2048)");
            if (s.size() > 65536) throw std::runtime_error("string::regex_match_all(): input too long (max 65536)");
            const std::regex& re = cached_regex(pat);
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
                auto match = std::make_shared<ValueArray>();
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
            // BYTECODE_FN is also a function (VM closure, named function). OLD BUG: is_function
            // was fixed, of was forgotten — in the VM it returned "unknown" for every function.
            case Value::BYTECODE_FN: return Value(std::string("function"));
            case Value::ARRAY:    return Value(std::string("array"));
            case Value::STRUCT:   return Value(std::string("struct"));
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
    // Is it a dictionary (not a list)? `[]` and json::decode("[]") are lists, json::decode("{}") is a dictionary; a struct and
    // null are not dictionaries. type::of says "array" for both (unchanged: so as not to break existing code).
    m.functions["is_map"]      = [](auto a) { check_args("type::is_map",      a.size(),1); return Value(look::value_is_map(a[0])); };
    // A function value is BYTECODE_FN (VM closure). The TYPE_OF opcode also says "function" for it;
    // is_function must be aligned with it.
    m.functions["is_function"] = [](auto a) { check_args("type::is_function", a.size(),1);
        return Value(a[0].type()==Value::BYTECODE_FN); };
    m.functions["to_int"]    = [](auto a) { check_args("type::to_int",    a.size(),1); return look_explicit_int(a[0]); };
    m.functions["to_float"]  = [](auto a) { check_args("type::to_float",  a.size(),1); return look_explicit_float(a[0]); };
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
        return Value(look::html_escape(args[0].to_string()));   // single definition: look/html_escape.h
    };

    m.functions["attr"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        out.reserve(s.size() * 4);
        // OWASP attribute-encode: encode all ASCII characters OTHER than alphanumerics
        // as &#xHH;. html::escape only escapes `<>&"'` — this is INSUFFICIENT in an
        // UNQUOTED attribute (injecting space/=/backtick/tab can open a new
        // event handler → XSS). Here everything including space/=/backtick is
        // encoded → safe in both quoted and unquoted attribute contexts.
        // Since the browser decodes the entity, a legitimate value is not corrupted (URL/data work).
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c >= 0x80) {
                out += (char)c;                  // alphanumeric + multi-byte UTF-8: safe
            } else {
                char buf[8];
                snprintf(buf, sizeof(buf), "&#x%02X;", c);
                out += buf;
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
        // working set size on Windows
        // Simple: just log a message
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
// ── parallel:: module ─────────────────────────────────────────────────────────
// Observability + control for tasks spawned with parallel().

static Module make_parallel_module() {
    Module m;
    m.name = "parallel";

    // parallel::active() → int — current task count
    m.functions["active"] = [](auto /*args*/) -> Value {
        return Value(task_active());
    };

    // parallel::wait([timeout_ms=5000]) → bool — true if all finished in time
    m.functions["wait"] = [](auto args) -> Value {
        int ms = 5000;
        if (!args.empty() && args[0].type() == Value::INT)
            ms = (int)args[0].as_int();
        return Value(task_wait(ms));
    };

    // parallel::limit() → int — max allowed tasks (LOOK_PARALLEL_LIMIT)
    m.functions["limit"] = [](auto /*args*/) -> Value {
        return Value(task_limit());
    };

    // parallel::at_capacity() → bool — non-throwing capacity check
    // Use to check before parallel() when you want to shed load gracefully:
    //   if (!parallel::at_capacity()) { parallel(fn) }
    m.functions["at_capacity"] = [](auto /*args*/) -> Value {
        return Value(task_active() >= task_limit());
    };

    return m;
}

// Forward declarations — defined in their respective .cpp files
Module make_http_module(ModuleHost* interp);
Module make_cache_module();
Module make_queue_module();
Module make_jobs_module();
Module make_mail_module();

// ── error:: module ─────────────────────────────────────────────────────────────
// Typed errors for .lk catch blocks.
//
// Usage:
//   throw error::new("not_found", "User does not exist", 404);
//
// catch e {
//   if e.type == "not_found" { response::status(404); }
//   io::println(e.message);
//   io::println(e.code);  // → 404
// }

static Module make_error_module() {
    Module m;
    m.name = "error";

    // error::new(type, message [, code=0]) → throws; the value in catch is the built-in STRUCT
    // Error{type, message, code}: $e.type and $e["type"] are readable, json::encode writes a dictionary.
    // Cannot be imitated: error::is looks at the definition's IDENTITY — json::decode does not produce a struct, a hand-built
    // dictionary is not a struct, a user struct cannot be declared with the name "Error" (parser).
    // (It used to be a plain list: $e["type"] was null, error::is took a ["type","admin"] list for an error.)
    // If not caught the message is "type: message" (with the call chain).
    static const Value error_def = [] {
        auto d = std::make_shared<ValueArray>();
        for (const char* f : {"type", "message"}) {
            d->push_back(Value(std::string(f))); d->push_back(Value()); d->push_back(Value(std::string("string")));
        }
        d->push_back(Value(std::string("code"))); d->push_back(Value()); d->push_back(Value(std::string("int")));
        return Value(d);
    }();
    m.functions["new"] = [](auto args) -> Value {
        std::string type    = args.size() >= 1 ? args[0].to_string() : "error";
        std::string message = args.size() >= 2 ? args[1].to_string() : "";
        int         code    = args.size() >= 3 ? (int)args[2].to_int()  : 0;
        Value e = struct_new("Error", error_def, [](const std::string&) { return false; });
        auto& sv = *e.as_struct();
        sv[STRUCT_SLOT0 + 0] = Value(type);
        sv[STRUCT_SLOT0 + 1] = Value(message);
        sv[STRUCT_SLOT0 + 2] = Value(code);
        LookRuntimeError err{e};
        err.message = message.empty() ? type : type + ": " + message;
        throw err;
        return Value(); // unreachable
    };

    // Field of an error value: only the built-in Error struct (definition identity); no other value.
    auto field = [](const Value& e, int i) -> const Value* {
        if (e.type() != Value::STRUCT) return nullptr;
        const auto& sv = *e.as_struct();
        if (sv.size() != STRUCT_SLOT0 + 3 || sv[1].as_array().get() != error_def.as_array().get()) return nullptr;
        return &sv[STRUCT_SLOT0 + i];
    };

    // error::is(e, type) → bool — type check in catch
    m.functions["is"] = [field](auto args) -> Value {
        if (args.size() < 2) return Value(false);
        const Value* t = field(args[0], 0);
        return Value(t && t->to_string() == args[1].to_string());
    };

    // error::message(e) → string — the message of the error value; another value (throw "text") itself
    m.functions["message"] = [field](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        if (const Value* msg = field(args[0], 1)) return Value(msg->to_string());
        const Value& e = args[0];
        return e.type() == Value::STRING ? e : Value(e.to_string());
    };

    // error::code(e) → int
    m.functions["code"] = [field](auto args) -> Value {
        if (args.empty()) return Value(0);
        const Value* c = field(args[0], 2);
        return c ? Value((int)c->to_int()) : Value(0);
    };

    return m;
}

std::map<std::string, Module> make_stdlib(ModuleHost* interp) {
    std::map<std::string, Module> stdlib;
    auto add = [&](Module mod) { stdlib[mod.name] = std::move(mod); };
    add(make_math());
    add(make_string());
    add(make_type());
    add(make_html());
    add(make_log());
    add(make_file_module());
    add(make_date_module());
    add(make_http_module(interp));
    add(make_cache_module());
    add(make_queue_module());
    add(make_jobs_module());
    add(make_mail_module());
    add(make_parallel_module());
    add(make_error_module());
    return stdlib;
}

} // namespace look



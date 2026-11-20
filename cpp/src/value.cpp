#include "look/value.h"
#include "look/int_overflow.h"
#include "look/struct_types.h"
#include "look/parallel_runtime.h"   // channel deadlock detection (ChanWait)
#include "look/assoc_index.h"
#include "look/mem_budget.h"      // mem_reserve — budget before a large concatenation
#include "look/fiber.h"           // a fiber waiting on a channel (without holding the thread)
#include <algorithm>
#include <cstring>
#include <random>
#include <mutex>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <cerrno>
#include <cctype>
#include "look/parse_double.h"

namespace look {

[[noreturn]] void look_nesting_error() {
    throw std::runtime_error("Value nested too deeply: more than " + std::to_string(LOOK_MAX_NESTING) +
        " levels of arrays/structs inside each other (keep deep data flat, e.g. a list of rows with parent ids)");
}

// ── Non-recursive release (ValueArray / Closure destructor) ──────────────────
// Past the depth threshold the elements are not destroyed but MOVED to a list (vector move is O(1)); the outermost
// release (depth 0) drains the list in a loop. The list is a pointer + on demand (not a thread_local
// object): at thread exit the thread_local destruction order could have made values destroyed later
// write to a dead list.
// fiber-ok: destruction does not suspend — the counter and the list are written and read inside the same synchronous release,
// with no wait in between; when the release finishes the counter is 0 again, the list empty.
static constexpr int DROP_INLINE_DEPTH = 256;
static thread_local int t_drop_depth = 0;
static thread_local std::vector<std::vector<Value>>* t_drop_later = nullptr;

// Rare paths are out of line: when inlined the hot path started saving 6 registers (objdump).
#if defined(_MSC_VER)
#  define LOOK_COLD_FN __declspec(noinline)
#else
#  define LOOK_COLD_FN __attribute__((noinline, cold))
#endif

LOOK_COLD_FN static void drain_deferred() noexcept {
    while (!t_drop_later->empty()) {
        std::vector<Value> v = std::move(t_drop_later->back());
        t_drop_later->pop_back();
        t_drop_depth = 1;
        v.clear();   // children still descend at most DROP_INLINE_DEPTH, the rest goes to the list
        t_drop_depth = 0;
    }
    delete t_drop_later;
    t_drop_later = nullptr;
}

LOOK_COLD_FN static void defer_values(std::vector<Value>& v) noexcept {
    if (!t_drop_later) t_drop_later = new std::vector<std::vector<Value>>();
    t_drop_later->push_back(std::move(v));
}

void look_release_values(std::vector<Value>& v) noexcept {
    const int d = t_drop_depth;
    if (d >= DROP_INLINE_DEPTH) { defer_values(v); return; }
    t_drop_depth = d + 1;
    v.clear();
    t_drop_depth = d;
    if (d == 0 && t_drop_later) drain_deferred();
}

// ── Dictionary index (look/assoc_index.h) ─────────────────────────────────────
void look_assoc_index_free(AssocIndex* ix) noexcept { delete ix; }

// Per-process random 128-bit seed. random_device reads getrandom/urandom on Linux, RtlGenRandom on Windows.
// The seed is written nowhere; it only makes the hash unpredictable to an attacker.
namespace {
struct SipKey { uint64_t k0, k1; };
const SipKey& sip_key() {
    // If the seed cannot be obtained we DO NOT FALL BACK to a constant: a predictable hash = hash flooding returns.
    // The process stops with a clear message (a machine without a random source cannot generate sessions/CSRF anyway).
    static const SipKey k = [] {
        try {
            std::random_device rd;
            auto w = [&] { return ((uint64_t)rd() << 32) ^ (uint64_t)rd(); };
            return SipKey{ w(), w() };
        } catch (const std::exception& e) {
            std::fprintf(stderr, "LOOK cannot start: no source of random numbers for the map hash key (%s)\n", e.what());
            std::abort();
        }
    }();
    return k;
}
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define LOOK_SIPROUND do { v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); \
                           v2 += v3; v3 = rotl(v3, 16); v3 ^= v2; \
                           v0 += v3; v3 = rotl(v3, 21); v3 ^= v0; \
                           v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); } while (0)
} // namespace

// SipHash-1-3 (Rust's HashMap default): 1 compression, 3 finalization rounds.
uint64_t look_key_hash(const char* p, size_t n) noexcept {
    const SipKey& key = sip_key();
    uint64_t v0 = 0x736f6d6570736575ULL ^ key.k0, v1 = 0x646f72616e646f6dULL ^ key.k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ key.k0, v3 = 0x7465646279746573ULL ^ key.k1;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
    const size_t full = n & ~(size_t)7;
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m;
        std::memcpy(&m, s + i, 8);   // little-endian machines (x86-64/arm64)
        v3 ^= m; LOOK_SIPROUND; v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t i = 0; i < (n & 7); ++i) b |= (uint64_t)s[full + i] << (8 * i);
    v3 ^= b; LOOK_SIPROUND; v0 ^= b;
    v2 ^= 0xff;
    LOOK_SIPROUND; LOOK_SIPROUND; LOOK_SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
#undef LOOK_SIPROUND

// ── Value ─────────────────────────────────────────────────────────────────────

// Double → string: short but full round-trip representation (std::to_chars shortest).
// The default ostream truncates to 6 significant digits and for large/small numbers
// escapes to scientific notation (123456789.123 → "1.23457e+08") — data loss for money/IDs.
// to_chars shortest: only the needed digits, fixed notation at reasonable magnitudes.
// look_format_double → look/format.h (the language's single double format; DB drivers use it too)

// Text INSIDE a list/dictionary/struct is quoted in LOOK syntax: [1, "a", "1"], ["k" => 3, "n" => "3"].
// Unquoted, 3 and "3" looked the same (types got confused while debugging — Node/Python/Rust also
// quote). Standalone text (print("x")) stays unquoted: only nested display.
static void append_nested(std::string& s, const Value& v) {
    if (v.type() != Value::STRING) { s += v.to_string(); return; }
    const std::string& t = v.str_ref();
    s += '"';
    for (char c : t) {
        switch (c) {
            case '"':  s += "\\\""; break;
            case '\\': s += "\\\\"; break;
            case '\n': s += "\\n"; break;
            case '\r': s += "\\r"; break;
            case '\t': s += "\\t"; break;
            default:   s += c;
        }
    }
    s += '"';
}

std::string Value::to_string() const {
    switch (type_) {
        case INT:    return std::to_string(int_val);
        case FLOAT:  return look_format_double(float_val);
        case STRING: return str_ref();
        case BOOL:   return bool_val ? "true" : "false";
        case NONE:   return "null";
        case ARRAY: {
            NestingGuard g;
            auto arr = as_array();
            std::string s = "[";
            // A dictionary in LOOK's syntax: ["k" => 3, "name" => "x"].
            // The internal marker used to show: [__assoc__, k, 3, name, x].
            if (!arr->empty() && (*arr)[0].is_assoc_marker()) {
                for (size_t i = 1; i + 1 < arr->size(); i += 2) {
                    if (i > 1) s += ", ";
                    append_nested(s, (*arr)[i]);
                    s += " => ";
                    append_nested(s, (*arr)[i + 1]);
                }
                return s + "]";
            }
            for (size_t i = 0; i < arr->size(); ++i) {
                if (i) s += ", ";
                append_nested(s, (*arr)[i]);
            }
            return s + "]";
        }
        case STRUCT: {
            // User{name: "Ada", age: 18}
            NestingGuard g;
            const auto& sv = *as_struct();
            const auto& def = look::struct_def(sv);
            std::string s = look::struct_name(sv) + "{";
            for (size_t j = 0, i = 0; j + 2 < def.size(); j += 3, ++i) {
                if (i) s += ", ";
                s += def[j].str_ref() + ": ";
                append_nested(s, sv[look::STRUCT_SLOT0 + i]);
            }
            return s + "}";
        }
        case CHANNEL:   return "<channel>";
        case WEBSOCKET: return "<websocket>";
    }
    return "";
}

bool Value::is_truthy() const {
    switch (type_) {
        case BOOL:   return bool_val;
        case INT:    return int_val != 0;
        case FLOAT:  return float_val != 0.0;
        case STRING: return !str_ref().empty() && str_ref() != "0";
        case NONE:   return false;
        case ARRAY:   return !as_array()->empty();
        case CHANNEL:   return ptr_val != nullptr;
        case WEBSOCKET: return ptr_val != nullptr;
        default:        return true;
    }
}

double Value::to_float() const {
    switch (type_) {
        case INT:    return (double)int_val;
        case FLOAT:  return float_val;
        case STRING: { double d = 0.0; parse_double(str_ref(), d); return d; }
        case BOOL:   return bool_val ? 1.0 : 0.0;
        default:     return 0.0;
    }
}

int64_t Value::to_int() const {
    switch (type_) {
        case INT:    return int_val;
        // Out-of-range / NaN double → int64 conversion is UNDEFINED behavior in C++ (previously int(1e19) →
        // INT64_MIN was measured). Defined clamping: NaN → 0, out of range → nearest bound (Rust `as`).
        case FLOAT:
            if (std::isnan(float_val)) return 0;
            if (float_val >= 9223372036854775808.0) return INT64_MAX;
            if (float_val < -9223372036854775808.0) return INT64_MIN;
            return (int64_t)float_val;
        // Non-numeric text → 0 (lax rule, unchanged). But an integer text that does NOT FIT in 64 bits
        // also used to give 0: list["99999999999999999999"] silently gave element 0, bit operations
        // silently gave 0. Now an explicit error (Go strconv: ErrRange; same rule as int()).
        case STRING:
            try { return std::stoll(str_ref()); }
            catch (const std::out_of_range&) {
                throw std::runtime_error("\"" + str_ref() + "\" does not fit in 64 bits (the range is "
                    "-9223372036854775808 to 9223372036854775807); keep a large ID as a string");
            }
            catch (...) { return 0; }
        case BOOL:   return bool_val ? 1 : 0;
        default:     return 0;
    }
}

// If int64 arithmetic overflows it is signed-overflow UB (9e18+9e18). Overflow detection is
// in look/int_overflow.h (pure, table-testable) — on overflow the caller promotes to float.
using look::i64_add_ovf;
using look::i64_sub_ovf;
using look::i64_mul_ovf;
// B-05: in arithmetic, throw an error instead of SILENTLY treating a string that cannot be converted to a number as 0.
// In LOOK == is type-strict ("5"==5 → false) but + was lax: because of to_int("abc") catch→0
// "abc"+1=1 and (since an int64-overflowing number in JSON is a STRING) bignum-string+1=1 —
// silent DATA CORRUPTION. A fully numeric string ("5") keeps working; a garbage string throws.
// to_int/to_float stay GENERAL (lenient-0 is wanted elsewhere); only arithmetic is strict.
// Checks one operand of + - * / % ** and says which path it takes: true = the float path. A string holding a
// decimal or exponent number ("2.5", "1e3") takes the float path, like a float. It used to be checked as a
// number and then read with to_int: "2.5" + 1 was 3 and "1e3" + 1 was 2 — a silently wrong result for any
// form field or query parameter holding a decimal.
static bool arith_float(const Value& v) {
    switch (v.type()) {
        case Value::INT: case Value::BOOL:
            return false;              // numeric — allowed (true=1/false=0, as in most languages)
        case Value::FLOAT:
            return true;
        case Value::STRING: break;     // validate the content below
        default:
            // NONE(null — MISSING ASSOC KEY!), ARRAY, FUNCTION, CHANNEL/WS/SSE:
            // not numbers. null+1=1 and []+1=1 are siblings of the "abc"+1=1 silent-0 bug —
            // and a missing key is far more common. Fail-loud (+ should be so too, as == is type-strict).
            throw std::runtime_error("Arithmetic on a non-numeric value (null/array/object)");
    }
    std::string s = v.to_string();
    if (s.empty())
        throw std::runtime_error("Arithmetic on an empty string");
    // No space around the number, at either end (Go's strconv): " 5" + 1 used to be 6 (stoll skips leading
    // space) while "5 " + 1 was an error. A form value with spaces needs string::trim first.
    if (std::isspace((unsigned char)s.front()) || std::isspace((unsigned char)s.back()))
        throw std::runtime_error("Arithmetic on a string with spaces around the number: '" + s +
                                 "' (use string::trim first)");
    size_t pos = 0;
    // 1) A full int64 string → the int path ("5"+1=6)
    try {
        (void)std::stoll(s, &pos);
        if (pos == s.size()) return false;
    } catch (const std::out_of_range&) {
        // An INTEGER string EXCEEDING int64 (bignum / Snowflake ID / BIGINT UNSIGNED).
        // JSON keeps an int64-overflowing number as a STRING for precision; arithmetic with it
        // fell to to_int→0 and gave a SILENTLY wrong result ("bignum + 1 = 1"). Reject.
        throw std::runtime_error("Arithmetic on a numeric string exceeding the int64 limit (bignum/ID): '" + s + "'");
    } catch (...) {}
    // 2) A decimal/exponent number string ("5.5", "1e3") → the float path. Only decimal notation: strtod also
    //    reads hex ("0x10"), "inf" and "nan", which are not numbers here.
    if (s.find_first_of("xXiInN") == std::string::npos) {
        double d;
        if (parse_double(s, d, &pos) && pos == s.size()) return true;
    }
    // 3) Not a number ("abc", "12x") → reject (so that + does not give a silent 0 while == is type-strict)
    throw std::runtime_error("Arithmetic on a string that cannot be converted to a number: '" + s + "'");
}
Value look_explicit_int(const Value& v) {
    if (v.type() != Value::STRING) return Value(v.to_int());
    const std::string& s = v.str_ref();
    try { return Value((int64_t)std::stoll(s)); }
    catch (const std::out_of_range&) {
        throw std::runtime_error("int(): \"" + s + "\" does not fit in 64 bits (the range is "
            "-9223372036854775808 to 9223372036854775807); keep a large ID as a string");
    }
    catch (...) { return Value((int64_t)0); }   // not a number: the lax rule (int("abc") == 0) is preserved
}
Value look_explicit_float(const Value& v) {
    if (v.type() != Value::STRING) return Value(v.to_float());
    const std::string& s = v.str_ref();
    // strtod: overflow ±HUGE_VAL + ERANGE → error (Go ParseFloat: ±Inf + ErrRange); underflow (1e-400)
    // is the nearest value (0) — not an error in Go either. stod used to return 0 for both.
    const char* p = s.c_str(); char* end = nullptr;
    errno = 0;
    double d = std::strtod(p, &end);
    if (end == p) return Value(0.0);            // not a number: the lax rule is preserved
    if (errno == ERANGE && std::isinf(d))
        throw std::runtime_error("float(): \"" + s + "\" is too large for a 64-bit float");
    return Value(d);
}

// Integer overflow is an EXPLICIT ERROR (Swift, Rust debug, C# checked). Previously the result silently
// fell to double (the PHP/JS model): a result such as 9223372036854775807 - (-2) lost its precision
// (…808 instead of …810) — an unnoticed wrong value in money / ID computation. Go wraps (that is also a
// silent wrong). Whoever wants an approximate result computes with a decimal (1.0 * a).
[[noreturn]] void look_int_overflow(int64_t a, const char* op, int64_t b) {
    throw std::runtime_error("Integer overflow: " + std::to_string(a) + " " + op + " " + std::to_string(b) +
        " does not fit in 64 bits (use a float, e.g. 1.0 * x, if an approximate result is fine)");
}
Value Value::operator+(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);   // | : both operands are checked
    if (fl) return Value(to_float() + o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_add_ovf(a, b, &r)) look_int_overflow(a, "+", b);
    return Value(r);
}
Value Value::operator-(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);   // | : both operands are checked
    if (fl) return Value(to_float() - o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_sub_ovf(a, b, &r)) look_int_overflow(a, "-", b);
    return Value(r);
}
Value Value::operator*(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);   // | : both operands are checked
    if (fl) return Value(to_float() * o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_mul_ovf(a, b, &r)) look_int_overflow(a, "*", b);
    return Value(r);
}
Value Value::operator/(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);
    double d = o.to_float();
    if (d == 0.0) throw std::runtime_error("Division by zero");  // the caller adds the source location
    if (fl) return Value(to_float() / d);
    int64_t i = o.to_int();
    int64_t self_i = to_int();  // not the raw int_val — so STRING/BOOL are also converted correctly ("6"/2=3)
    // INT64_MIN / -1: divides exactly but the result (2^63) does not fit in int64 — overflow (UB in C++).
    if (i == -1 && self_i == INT64_MIN) look_int_overflow(self_i, "/", i);
    if (i != 0 && self_i % i == 0) return Value(self_i / i);
    return Value(to_float() / d);
}
Value Value::operator%(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);
    // Decimal operand: floating-point remainder, sign from the dividend (Go math.Mod, C fmod, JS, Java).
    // Previously both sides were converted to integers: 7.5 % 2 → 1 (the PHP model; the fractional part silently vanished).
    if (fl) {
        double d = o.to_float();
        if (d == 0.0) throw std::runtime_error("Modulo by zero");
        return Value(std::fmod(to_float(), d));
    }
    int64_t i = o.to_int();
    if (i == 0) throw std::runtime_error("Modulo by zero");
    if (i == -1) return Value((int64_t)0);  // INT64_MIN % -1 UB; a % -1 is always 0
    return Value(to_int() % i);
}
// Power: integer ** non-negative integer → EXACT integer (Python, Ruby); an error on overflow (same
// rule as addition). It used to always compute in double: above 2^53 the result was silently approximate. A negative exponent
// or a decimal operand → double. 0 ** negative → division by zero (Python; previously silently Infinity).
Value Value::pow(const Value& o) const {
    const bool fl = arith_float(*this) | arith_float(o);
    if (!fl) {
        int64_t base = to_int(), e = o.to_int();
        if (e < 0) {
            if (base == 0) throw std::runtime_error("Division by zero: 0 ** " + std::to_string(e));
            return Value(std::pow((double)base, (double)e));
        }
        int64_t result = 1, b = base;
        int64_t n = e;
        while (n > 0) {
            if (n & 1) { int64_t r; if (i64_mul_ovf(result, b, &r)) look_int_overflow(base, "**", e); result = r; }
            n >>= 1;
            if (n > 0) { int64_t r; if (i64_mul_ovf(b, b, &r)) look_int_overflow(base, "**", e); b = r; }
        }
        return Value(result);
    }
    double x = to_float(), y = o.to_float();
    if (x == 0.0 && y < 0) throw std::runtime_error("Division by zero: 0 ** negative");
    return Value(std::pow(x, y));
}
// The new text is EXACTLY the right size (OLD: while appending to a to_string() + … temporary copy the capacity doubled:
// 1 MiB . "x" = 2 MiB). The text operand is read without being copied.
Value Value::concat(const Value& o) const {
    std::string ls, rs;
    const std::string& l = (type_ == STRING && ptr_val) ? *static_cast<const std::string*>(ptr_val.get())
                                                        : (ls = to_string());
    const std::string& r = (o.type_ == STRING && o.ptr_val) ? *static_cast<const std::string*>(o.ptr_val.get())
                                                            : (rs = o.to_string());
    // A large concatenation asks the budget beforehand: `$s = $s . $s` can allocate hundreds of MiB in a single step,
    // without waiting for a safe point (look/mem_budget.h).
    const size_t n = l.size() + r.size();
    if (n >= (size_t(1) << 20)) mem_reserve((int64_t)n, "joining strings with .");
    std::string out;
    out.reserve(n);
    out.append(l).append(r);
    return Value(std::move(out));
}

void Value::append_in_place(const Value& o) {
    // o.to_string() is copied first → aliasing such as `$s .= $s` is safe.
    std::string rhs = o.to_string();
    if (type_ != STRING || !ptr_val) {
        ptr_val = std::make_shared<std::string>(to_string());  // number/none → new buffer
        type_ = STRING;
    } else {
        // COW: if the string is shared (constant pool / aliased register) take a private copy
        // before mutating — in B5, behind the string pointer, in-place mutation would
        // corrupt the other Values sharing it. After the first append it is unique → amortized O(1).
        // NOTE: ptr_val.use_count() is used directly — an intermediate shared_ptr copy
        // creates an extra owner and makes use_count() always >1 (→ every step COW = O(n²)).
        // != 1: constant text without a counter (the dictionary marker, use_count()==0) is also copied — if
        // modified in place all dictionaries would be corrupted.
        if (ptr_val.use_count() != 1)
            ptr_val = std::make_shared<std::string>(*static_cast<std::string*>(ptr_val.get()));
    }
    std::string* buf = static_cast<std::string*>(ptr_val.get());
    // If the growth (at most double) exceeds 1 MiB the budget is asked beforehand (look/mem_budget.h).
    const size_t need = buf->size() + rhs.size();
    if (need > buf->capacity() && need >= (size_t(1) << 20))
        mem_reserve((int64_t)std::max(need, buf->capacity() * 2), "joining strings with .=");
    buf->append(rhs);
}

// Python-like strict comparison: NO coercion between types. (Not Go — Go
// gives a compile error even for int↔float; we compare numeric types: 1==1.0.)
//   0 == "abc"  → false   (previously true — it coerced "abc"→0; a footgun)
//   "5" == 5    → false   (string and number are separate types)
//   5 == 5.0    → true    (INT/FLOAT is a single "number" type — separating them in a dynamic language
//                          would be a worse footgun: the DB gives 5, a calculation gives 5.0)
// Form input arrives as a string; to compare it with a number it is first converted with int()/float()
// (explicit and safe — the dynamic counterpart of Go's static typing requirement).
static inline bool val_is_number(Value::Type t) { return t == Value::INT || t == Value::FLOAT; }

// LOOK: arrays, maps and structs are compared by CONTENT (recursively). With value semantics
// there is no such thing as identity; `$a == $b` means "the same value", not "the same thing".
//   list   : same length + equal elements in order
//   map    : same keys + equal values; INSERTION ORDER does not matter
//   struct : same name + equal fields
// An empty list and an empty map are equal ([] can also be written as a map that has not yet received a key).
static bool vec_is_map(const std::vector<Value>& v) {
    return !v.empty() && v[0].is_assoc_marker();
}
static bool vec_content_equal(const std::vector<Value>& a, const std::vector<Value>& b) {
    const bool am = vec_is_map(a), bm = vec_is_map(b);
    if (am != bm) return (am ? a.size() == 1 && b.empty() : b.size() == 1 && a.empty());
    if (a.size() != b.size()) return false;
    if (!am) {
        for (size_t i = 0; i < a.size(); ++i) if (!(a[i] == b[i])) return false;
        return true;
    }
    for (size_t i = 1; i + 1 < a.size(); i += 2) {
        // Fast path: in two maps built in the same order the key is at the same position.
        if (a[i] == b[i]) { if (!(a[i + 1] == b[i + 1])) return false; continue; }
        bool found = false;
        for (size_t j = 1; j + 1 < b.size(); j += 2)
            if (a[i] == b[j]) { if (!(a[i + 1] == b[j + 1])) return false; found = true; break; }
        if (!found) return false;
    }
    return true;
}
// Number ↔ number EXACT comparison: -1 / 0 / 1; if there is a NaN, unordered=true (equal to nothing,
// no ordering is true — IEEE / Go). Mixed int↔double: if the integer part of the double is in the int64 range it is
// compared as an integer and the remaining fraction is compared separately (the Python model). OLD: a mixed pair was converted to
// double → precision loss above 2^53: 9007199254740993 == 9007199254740992.0 → true (measured).
static int num_cmp(const Value& a, const Value& b, bool& unordered) {
    unordered = false;
    const bool ai = a.type() == Value::INT, bi = b.type() == Value::INT;
    if (ai && bi) return a.as_int() < b.as_int() ? -1 : a.as_int() > b.as_int() ? 1 : 0;
    if (!ai && !bi) {                                   // double ↔ double (bool → 0/1)
        double x = a.to_float(), y = b.to_float();
        if (std::isnan(x) || std::isnan(y)) { unordered = true; return 0; }
        return x < y ? -1 : x > y ? 1 : 0;
    }
    const int64_t i = ai ? a.as_int() : b.as_int();
    const double  d = ai ? b.to_float() : a.to_float();
    int r;                                              // comparison of i with d
    if (std::isnan(d)) { unordered = true; return 0; }
    if (d >= 9223372036854775808.0)       r = -1;      // d is greater than every int64 (including +Inf)
    else if (d < -9223372036854775808.0)  r = 1;       // d is less than every int64 (including -Inf)
    else {
        const double t = std::trunc(d);
        const int64_t ti = (int64_t)t;                  // within range: a defined conversion
        if (i != ti) r = i < ti ? -1 : 1;
        else { const double f = d - t; r = f > 0 ? -1 : f < 0 ? 1 : 0; }
    }
    return ai ? r : -r;
}

bool Value::operator==(const Value& o) const {
    bool a_num = val_is_number(type_), b_num = val_is_number(o.type_);
    if (a_num && b_num) {
        // int↔int: compare EXACT int64. Converting to double with to_float() makes distinct int64s
        // above 2^53 equal (9007199254740993==...992, INT64_MAX-1==INT64_MAX →
        // true) — both a language-level == mistake (in v1, on both engines, invisible to the differential), and
        // VM constant-pool dedup corruption (compiler.cpp dedup uses this == → int
        // literals collide and get corrupted). Mixed int/float is exact too (num_cmp).
        if (type_ == INT && o.type_ == INT) return int_val == o.int_val;
        bool un; int c = num_cmp(*this, o, un);
        return !un && c == 0;
    }
    if (type_ != o.type_) return false;                     // different type → never equal
    switch (type_) {
        case STRING: return str_ref() == o.str_ref();
        case BOOL:   return bool_val == o.bool_val;
        case NONE:   return true;                           // null == null
        case ARRAY: {
            if (ptr_val == o.ptr_val) return true;          // same storage (a copy not yet separated)
            if (!ptr_val || !o.ptr_val) return false;
            NestingGuard g;
            return vec_content_equal(*vec_ptr(), *o.vec_ptr());
        }
        case STRUCT: {
            if (ptr_val == o.ptr_val) return true;
            if (!ptr_val || !o.ptr_val) return false;
            const auto& a = *vec_ptr(); const auto& b = *o.vec_ptr();
            if (a.size() != b.size() || a[0].str_ref() != b[0].str_ref()) return false;
            NestingGuard g;
            for (size_t i = 2; i < a.size(); ++i) if (!(a[i] == b[i])) return false;
            return true;
        }
        default:     return ptr_val == o.ptr_val;           // fn/channel/ws: identity (handles)
    }
}
// Ordering: only number↔number and string↔string. Mixed type → error (instead of silently
// coercing the string to 0 and giving a wrong result, surface the bug).
[[noreturn]] static void cmp_type_error() {
    throw std::runtime_error(
        "Incomparable types: '<'/'>' only for number↔number and string↔string — "
        "convert with int()/float()/string() before comparing different types");
}
// Numeric ordering: int↔int and mixed int/float are EXACT (num_cmp; distinct values above 2^53
// would be equal in double). Every ordering with NaN is false (IEEE / Go).
bool Value::operator<(const Value& o)  const {
    if (type_ == INT && o.type_ == INT) return int_val < o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) { bool un; int c = num_cmp(*this, o, un); return !un && c < 0; }
    if (type_ == STRING && o.type_ == STRING) return str_ref() < o.str_ref();
    cmp_type_error();
}
bool Value::operator<=(const Value& o) const {
    if (type_ == INT && o.type_ == INT) return int_val <= o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) { bool un; int c = num_cmp(*this, o, un); return !un && c <= 0; }
    if (type_ == STRING && o.type_ == STRING) return str_ref() <= o.str_ref();
    cmp_type_error();
}
bool Value::operator>(const Value& o)  const {
    if (type_ == INT && o.type_ == INT) return int_val > o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) { bool un; int c = num_cmp(*this, o, un); return !un && c > 0; }
    if (type_ == STRING && o.type_ == STRING) return str_ref() > o.str_ref();
    cmp_type_error();
}
bool Value::operator>=(const Value& o) const {
    if (type_ == INT && o.type_ == INT) return int_val >= o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) { bool un; int c = num_cmp(*this, o, un); return !un && c >= 0; }
    if (type_ == STRING && o.type_ == STRING) return str_ref() >= o.str_ref();
    cmp_type_error();
}
int  Value::spaceship(const Value& o)  const { return (*this == o) ? 0 : (*this < o ? -1 : 1); }

Value Value::bitwise_and(const Value& o) const { return Value(to_int() & o.to_int()); }
Value Value::bitwise_or(const Value& o)  const { return Value(to_int() | o.to_int()); }
Value Value::bitwise_xor(const Value& o) const { return Value(to_int() ^ o.to_int()); }
Value Value::bitwise_not()               const { return Value(~to_int()); }
// If the shift amount is outside [0,63] (1<<64, 1<<-1) it is UB — out of range → 0.
// Unsigned is used for left shifts (avoids signed-overflow UB).
// A negative shift count is an explicit error (Go: "negative shift amount"). It used to silently return 0.
[[noreturn]] static void negative_shift(int64_t n) {
    throw std::runtime_error("Negative shift count: " + std::to_string(n));
}
Value Value::shift_left(const Value& o)  const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0) negative_shift(n);
    if (n >= 64) return Value((int64_t)0);   // Go: as many as the integer width or more → 0
    return Value((int64_t)((uint64_t)a << n));
}
Value Value::shift_right(const Value& o) const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0) negative_shift(n);
    // Arithmetic shift: a shift greater than 63 fills with the sign bit (Go: -8 >> 70 == -1). It used to return
    // 0 — -8 >> 63 == -1 while -8 >> 64 == 0 was inconsistent.
    if (n >= 64) return Value((int64_t)(a < 0 ? -1 : 0));
    return Value((int64_t)(a >> n));
}

// ── LookChannel methods ───────────────────────────────────────────────────────

// Channel operation watchdog — if LOOK_CHANNEL_TIMEOUT_MS is set (>0) send/recv
// throw a catchable error when this time is exceeded instead of hanging forever. So
// if there is no sender/receiver to match (a deadlock caused by a logic error) the worker is not
// locked up forever; it escapes. Unset/0 = block forever (Go unbuffered rendezvous default).
// In a single place — the VM (CHAN_SEND/RECV) calls these methods.
static long channel_timeout_ms() {
    static const long v = []() -> long {
        const char* e = std::getenv("LOOK_CHANNEL_TIMEOUT_MS");
        if (e && *e) { long n = std::atol(e); if (n > 0) return n; }
        return 0;
    }();
    return v;
}

// Channel wait (three places: send on a full buffer, delivery of an unbuffered send, receive). If ready it does not
// wait; if waiting it enters the deadlock accounting (parallel_runtime.h ChanWait) — if the script and
// all tasks are waiting on channels they all wake up and get a "deadlock" error, never hanging forever.
void LookChannel::wake_fibers(std::vector<FiberWaiter>& list) {
    // Lock order: channel mtx → scheduler mtx_ (resume_fiber). The scheduler does not take the channel lock.
    for (auto& w : list) { *w.queued = false; w.sched->resume_fiber(std::move(w.fiber)); }
    list.clear();
}

template <class Ready>
static void chan_wait(LookChannel* ch, std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                      std::vector<LookChannel::FiberWaiter>& fw,
                      long to, int kind, uint64_t gen, Ready ready, const char* timeout_msg) {
    if (ready()) return;
    if (g_chan_deadlock.load()) throw std::runtime_error(chan_deadlock_message());
    ChanWait w(ch->shared_from_this(), kind, gen);
    if (w.suspect) { lk.unlock(); chan_deadlock_try(); lk.lock(); }   // the decision is made without channel locks
    auto pred = [&] { return ready() || g_chan_deadlock.load(); };

    // Inside a fiber: make only this fiber wait, not the thread (LookChannel::FiberWaiter).
    Fiber* cur = Fiber::current();
    FiberScheduler* sched = cur ? get_thread_scheduler() : nullptr;
    std::shared_ptr<Fiber> self = (cur && sched) ? cur->shared_from_this_fiber() : nullptr;
    if (self) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(to > 0 ? to : 0);
        while (!pred()) {
            if (to > 0) {
                // Timed wait (LOOK_CHANNEL_TIMEOUT_MS): NO registration — if a fiber suspended with a timer were also resumed by
                // another thread it would be queued twice. It looks again at short
                // intervals (≤ 2 ms); only when this setting is on.
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) throw std::runtime_error(timeout_msg);
                int step = (int)std::min<long long>(2, left);
                lk.unlock();
                int r = sched->wait_readable_tmo(self, -1, step);
                lk.lock();
                if (r < 0) cv.wait_for(lk, std::chrono::milliseconds(step), pred);   // no timer
                continue;
            }
            bool queued = true;
            fw.push_back({self, sched, &queued});
            lk.unlock();
            for (;;) {
                FiberScheduler::suspend_current();
                lk.lock();
                if (!queued) break;   // whoever woke us removed our registration
                lk.unlock();          // spurious early resume (left over from an earlier wait): keep waiting
            }
        }
        if (!ready()) throw std::runtime_error(chan_deadlock_message());
        return;
    }
    if (to > 0) {
        if (!cv.wait_for(lk, std::chrono::milliseconds(to), pred)) throw std::runtime_error(timeout_msg);
    } else {
        cv.wait(lk, pred);
    }
    if (!ready()) throw std::runtime_error(chan_deadlock_message());
}

std::shared_ptr<LookChannel> make_channel(const std::vector<Value>& args) {
    if (args.empty()) return std::make_shared<LookChannel>(128);
    if (args[0].type() != Value::INT)
        throw std::runtime_error("channel(): the size must be a whole number (0 = unbuffered)");
    const int64_t n = args[0].as_int();
    if (n < 0) throw std::runtime_error("channel: capacity cannot be negative");
    return std::make_shared<LookChannel>((size_t)n);   // 0 → unbuffered (LookChannel)
}

void LookChannel::send_val(Value val, std::atomic<int>* phase) {
    std::unique_lock<std::mutex> lk(mtx);
    long to = channel_timeout_ms();
    chan_wait(this, lk, not_full, fiber_send, to, CHAN_WAIT_ROOM, 0, [this]{ return closed || queue.size() < capacity; },
              "channel send timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
    if (closed) throw std::runtime_error("send on closed channel");
    // ISOLATION (same as the cache/parallel deep_clone discipline): the channel is parallel's
    // cross-thread communication primitive. If the Value is an ARRAY/MAP the shared_ptr<container> is SHARED
    // with the sender — the sender still holds $arr after send (LOOK is by-reference),
    // and the receiver gets the same vector → sender+receiver mutating the same vector concurrently =
    // heap race (twin of the cache's; TSan push_back/_M_realloc_insert). A deep_clone snapshot
    // → the queue holds an isolated value, sender and receiver are decoupled. Since it is single-receiver (pop)
    // a single clone on send is enough (the cache is multi-reader so it also cloned on get).
    Value item = val.deep_clone();
    // Delivery moment (parallel_runtime.h): with a buffered channel the receiver may wake after this push → first
    // RUNNING; with an unbuffered one the sender waits for the receiver → the receiver pulls to RUNNING while receiving (recv_val).
    if (phase) { if (unbuffered) rv_phase = phase; else phase->store(0); }
    queue.push(std::move(item));
    not_empty.notify_one();
    wake_fibers(fiber_recv);
    if (unbuffered) {
        // On exit (received, closed or timed out) release our state pointer: it is deleted when the task
        // finishes, so a receiver that takes the item later must not write to it.
        struct ClearRv { LookChannel* c; std::atomic<int>* p;
            ~ClearRv() { if (p && c->rv_phase == p) c->rv_phase = nullptr; } } clear_rv{this, phase};
        // Rendezvous: block until a receiver takes THIS item (Go unbuffered channel).
        // We track that our own item was taken via recv_gen; on a spurious wakeup
        // (another sender) the wait continues.
        uint64_t gen = recv_gen;
        chan_wait(this, lk, not_full, fiber_send, to, CHAN_WAIT_TAKEN, gen, [this, gen]{ return closed || recv_gen > gen; },
                  "channel send (rendezvous) timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
        // Closed without being taken: the value was not delivered → like Go "send on closed channel" (the item
        // is taken back from the queue, does not reach a receiver after close).
        if (recv_gen == gen) {
            if (!queue.empty()) queue.pop();
            throw std::runtime_error("send on closed channel");
        }
    }
}

Value LookChannel::recv_val() {
    std::unique_lock<std::mutex> lk(mtx);
    long to = channel_timeout_ms();
    chan_wait(this, lk, not_empty, fiber_recv, to, CHAN_WAIT_RECV, 0, [this]{ return closed || !queue.empty(); },
              "channel receive timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
    if (queue.empty()) return Value();  // closed + empty → null
    Value v = std::move(queue.front());
    queue.pop();
    if (rv_phase) { rv_phase->store(0); rv_phase = nullptr; }   // send_val: delivery moment
    ++recv_gen;                 // rendezvous: release the waiting sender
    not_full.notify_all();      // multiple senders may be waiting
    wake_fibers(fiber_send);
    return v;
}

void LookChannel::close_chan() {
    std::unique_lock<std::mutex> lk(mtx);
    closed = true;
    not_empty.notify_all();
    not_full.notify_all();
    wake_fibers(fiber_recv);
    wake_fibers(fiber_send);
}

} // namespace look

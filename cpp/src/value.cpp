#include "look/value.h"
#include "look/int_overflow.h"
#include "look/struct_types.h"
#include <mutex>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <cerrno>

namespace look {

// ── Özyinelemesiz bırakma (ValueArray / Closure yıkıcısı) ─────────────────────
// Derinlik eşiği geçince elemanlar yıkılmaz, listeye TAŞINIR (vektör taşıma O(1)); en dıştaki
// bırakma (derinlik 0) listeyi döngüyle boşaltır. Liste işaretçi + talep üzerine (thread_local
// nesne değil): iş parçacığı kapanırken thread_local yıkım sırası, sonradan yıkılan değerlerin
// ölmüş bir listeye yazmasına yol açabilirdi. Askı yok: yıkım sırasında fiber geçişi olmaz.
static constexpr int DROP_INLINE_DEPTH = 256;
static thread_local int t_drop_depth = 0;
static thread_local std::vector<std::vector<Value>>* t_drop_later = nullptr;

// Nadir yollar satır dışı: içe gömülünce sıcak yol 6 yazmaç kaydetmeye başlıyordu (objdump).
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
        v.clear();   // çocuklar yine en fazla DROP_INLINE_DEPTH iner, fazlası listeye
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

// ── Value ─────────────────────────────────────────────────────────────────────

// Double → string: kısa ama tam round-trip gösterim (std::to_chars shortest).
// Varsayilan ostream 6 anlamli basamakla keser ve buyuk/kucuk sayida bilimsel
// gosterime kacar (123456789.123 → "1.23457e+08") — para/ID icin veri kaybi.
// to_chars shortest: gereken must be at least basamak, makul buyuklukte sabit gosterim.
// look_format_double → look/format.h (dilin tek double formatı; DB sürücüleri de kullanır)

std::string Value::to_string() const {
    switch (type_) {
        case INT:    return std::to_string(int_val);
        case FLOAT:  return look_format_double(float_val);
        case STRING: return str_ref();
        case BOOL:   return bool_val ? "true" : "false";
        case NONE:   return "null";
        case ARRAY: {
            auto arr = as_array();
            std::string s = "[";
            for (size_t i = 0; i < arr->size(); ++i) {
                if (i) s += ", ";
                s += (*arr)[i].to_string();
            }
            return s + "]";
        }
        case STRUCT: {
            // User{name: Ada, age: 18}
            const auto& sv = *as_struct();
            const auto& def = look::struct_def(sv);
            std::string s = look::struct_name(sv) + "{";
            for (size_t j = 0, i = 0; j + 2 < def.size(); j += 3, ++i) {
                if (i) s += ", ";
                s += def[j].str_ref() + ": " + sv[look::STRUCT_SLOT0 + i].to_string();
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
        case STRING: try { return std::stod(str_ref()); } catch(...) { return 0.0; }
        case BOOL:   return bool_val ? 1.0 : 0.0;
        default:     return 0.0;
    }
}

int64_t Value::to_int() const {
    switch (type_) {
        case INT:    return int_val;
        // Aralık dışı / NaN double → int64 dönüşümü C++'ta TANIMSIZ davranış (eskiden int(1e19) →
        // INT64_MIN ölçüldü). Tanımlı sınırlama: NaN → 0, aralık dışı → en yakın sınır (Rust `as`).
        case FLOAT:
            if (std::isnan(float_val)) return 0;
            if (float_val >= 9223372036854775808.0) return INT64_MAX;
            if (float_val < -9223372036854775808.0) return INT64_MIN;
            return (int64_t)float_val;
        case STRING: try { return std::stoll(str_ref()); } catch(...) { return 0; }
        case BOOL:   return bool_val ? 1 : 0;
        default:     return 0;
    }
}

// int64 aritmetiği taşarsa signed-overflow UB olur (9e18+9e18). Taşma-tespiti
// look/int_overflow.h'de (saf, tablo-test edilebilir) — taşınca çağıran float'a promote eder.
using look::i64_add_ovf;
using look::i64_sub_ovf;
using look::i64_mul_ovf;
// B-05: aritmetikte sayıya çevrilemeyen string'i SESSİZCE 0 kabul etmek yerine hata ver.
// LOOK'ta == tip-katı ("5"==5 → false) ama + gevşekti: to_int("abc") catch→0 yüzünden
// "abc"+1=1 ve (JSON'da int64-taşan sayı STRING olduğundan) bignum-string+1=1 —
// sessiz VERİ BOZULMASI. Tam sayısal string ("5") çalışmaya devam eder; çöp string atar.
// to_int/to_float GENEL kalır (başka yerlerde lenient-0 istenir); yalnız aritmetik katı.
static void arith_check(const Value& v) {
    switch (v.type()) {
        case Value::INT: case Value::FLOAT: case Value::BOOL:
            return;                    // sayısal — serbest (true=1/false=0, çoğu dilde böyle)
        case Value::STRING: break;     // içeriği aşağıda doğrula
        default:
            // NONE(null — EKSİK ASSOC ANAHTARI!), ARRAY, FUNCTION, CHANNEL/WS/SSE:
            // sayı değil. null+1=1 ve []+1=1, "abc"+1=1 sessiz-0 bug'ının kardeşi —
            // ve eksik anahtar çok daha yaygın. Fail-loud (== tip-katıyken + de olsun).
            throw std::runtime_error("Arithmetic on a non-numeric value (null/array/object)");
    }
    std::string s = v.to_string();
    if (s.empty())
        throw std::runtime_error("Arithmetic on an empty string");
    size_t pos = 0;
    // 1) Tam int64 string'i → geçerli (operator+ int yoluyla doğru işler: "5"+1=6)
    try {
        (void)std::stoll(s, &pos);
        if (pos == s.size()) return;
    } catch (const std::out_of_range&) {
        // int64'ü AŞAN TAMSAYI string'i (bignum / Snowflake ID / BIGINT UNSIGNED).
        // JSON int64-taşan sayıyı kesinlik için STRING tutuyor; onunla aritmetik
        // to_int→0'a düşüp SESSİZCE yanlış sonuç veriyordu ("bignum + 1 = 1"). Reddet.
        throw std::runtime_error("Arithmetic on a numeric string exceeding the int64 limit (bignum/ID): '" + s + "'");
    } catch (...) {}
    // 2) Ondalık/üslü sayı string'i ("5.5", "1e3") → geçerli (float yolu)
    try {
        (void)std::stod(s, &pos);
        if (pos == s.size()) return;
    } catch (...) {}
    // 3) Sayı değil ("abc", "12x") → reddet (== tip-katıyken + sessiz 0 vermesin)
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
    catch (...) { return Value((int64_t)0); }   // sayı değil: gevşek kural (int("abc") == 0) korunur
}
Value look_explicit_float(const Value& v) {
    if (v.type() != Value::STRING) return Value(v.to_float());
    const std::string& s = v.str_ref();
    // strtod: taşma ±HUGE_VAL + ERANGE → hata (Go ParseFloat: ±Inf + ErrRange); alttan taşma (1e-400)
    // en yakın değer (0) — Go'da da hata değil. Eskiden stod her ikisinde de 0 dönüyordu.
    const char* p = s.c_str(); char* end = nullptr;
    errno = 0;
    double d = std::strtod(p, &end);
    if (end == p) return Value(0.0);            // sayı değil: gevşek kural korunur
    if (errno == ERANGE && std::isinf(d))
        throw std::runtime_error("float(): \"" + s + "\" is too large for a 64-bit float");
    return Value(d);
}

// Tamsayı taşması AÇIK HATA (Swift, Rust hata ayıklama, C# checked). Eskiden sonuç sessizce double'a
// düşüyordu (PHP/JS modeli): 9223372036854775807 - (-2) gibi bir sonuç kesinliğini kaybediyordu
// (…810 yerine …808) — para / kimlik hesabında fark edilmeyen yanlış değer. Go sarmalar (o da
// sessiz yanlış). Yaklaşık sonuç isteyen ondalıkla hesaplar (1.0 * a).
[[noreturn]] void look_int_overflow(int64_t a, const char* op, int64_t b) {
    throw std::runtime_error("Integer overflow: " + std::to_string(a) + " " + op + " " + std::to_string(b) +
        " does not fit in 64 bits (use a float, e.g. 1.0 * x, if an approximate result is fine)");
}
Value Value::operator+(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() + o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_add_ovf(a, b, &r)) look_int_overflow(a, "+", b);
    return Value(r);
}
Value Value::operator-(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() - o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_sub_ovf(a, b, &r)) look_int_overflow(a, "-", b);
    return Value(r);
}
Value Value::operator*(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() * o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_mul_ovf(a, b, &r)) look_int_overflow(a, "*", b);
    return Value(r);
}
Value Value::operator/(const Value& o) const {
    arith_check(*this); arith_check(o);
    double d = o.to_float();
    if (d == 0.0) throw std::runtime_error("Division by zero");  // the caller adds the source location
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() / d);
    int64_t i = o.to_int();
    int64_t self_i = to_int();  // ham int_val değil — STRING/BOOL de doğru çevrilsin ("6"/2=3)
    // INT64_MIN / -1: tam bölünür ama sonuç (2^63) int64'e sığmaz — taşma (C++'ta UB).
    if (i == -1 && self_i == INT64_MIN) look_int_overflow(self_i, "/", i);
    if (i != 0 && self_i % i == 0) return Value(self_i / i);
    return Value(to_float() / d);
}
Value Value::operator%(const Value& o) const {
    arith_check(*this); arith_check(o);
    // Ondalık işlenen: kayan noktalı kalan, işareti bölünenden (Go math.Mod, C fmod, JS, Java).
    // Eskiden iki taraf tamsayıya çevriliyordu: 7.5 % 2 → 1 (PHP modeli; ondalık kısım sessizce gidiyordu).
    if (type_ == FLOAT || o.type_ == FLOAT) {
        double d = o.to_float();
        if (d == 0.0) throw std::runtime_error("Modulo by zero");
        return Value(std::fmod(to_float(), d));
    }
    int64_t i = o.to_int();
    if (i == 0) throw std::runtime_error("Modulo by zero");
    if (i == -1) return Value((int64_t)0);  // INT64_MIN % -1 UB; a % -1 daima 0
    return Value(to_int() % i);
}
// Üs: tamsayı ** negatif olmayan tamsayı → TAM tamsayı (Python, Ruby); taşarsa hata (toplama ile aynı
// kural). Eskiden her zaman double hesaplanıyordu: 2^53 üstünde sonuç sessizce yaklaşıktı. Negatif üs
// ya da ondalık işlenen → double. 0 ** negatif → sıfıra bölme (Python; eskiden sessizce Infinity).
Value Value::pow(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ != FLOAT && o.type_ != FLOAT) {
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
Value Value::concat(const Value& o)    const { return Value(to_string() + o.to_string()); }

void Value::append_in_place(const Value& o) {
    // o.to_string() önce kopyalanır → `$s .= $s` gibi aliasing güvenli.
    std::string rhs = o.to_string();
    if (type_ != STRING || !ptr_val) {
        ptr_val = std::make_shared<std::string>(to_string());  // sayı/none → yeni tampon
        type_ = STRING;
    } else {
        // COW: string paylaşılıyorsa (constant pool / aliased register) mutasyondan
        // önce özel kopya al — B5'te string pointer arkasında, in-place mutasyon
        // paylaşan diğer Value'ları bozardı. İlk append'ten sonra tekil → amortize O(1).
        // NOT: ptr_val.use_count() doğrudan kullanılır — ara shared_ptr kopyası
        // ekstra owner yaratıp use_count'u daima >1 yapar (→ her adım COW = O(n²)).
        if (ptr_val.use_count() > 1)
            ptr_val = std::make_shared<std::string>(*static_cast<std::string*>(ptr_val.get()));
    }
    static_cast<std::string*>(ptr_val.get())->append(rhs);
}

// Python-benzeri katı karşılaştırma: türler-arası coercion YOK. (Go değil — Go
// int↔float'a bile derleme hatası verir; biz sayısal türleri kıyaslarız: 1==1.0.)
//   0 == "abc"  → false   (eskiden true — "abc"→0 coerce ediyordu; footgun)
//   "5" == 5    → false   (string ve sayı ayrı tür)
//   5 == 5.0    → true    (INT/FLOAT tek "sayı" türü — ayırmak dinamik dilde
//                          daha kötü footgun olurdu: DB 5, hesap 5.0 verir)
// Form girdisi string gelir; sayıyla karşılaştırmak için önce int()/float() ile
// dönüştürülür (açık ve güvenli — Go'nun statik tip is requiredluğunun dinamik karşılığı).
static inline bool val_is_number(Value::Type t) { return t == Value::INT || t == Value::FLOAT; }

// LOOK 2: dizi, map ve struct İÇERİĞE göre karşılaştırılır (özyinelemeli). Değer semantiğinde
// kimlik diye bir şey yok; `$a == $b` "aynı şey mi" değil "aynı değer mi" demektir.
//   liste  : aynı uzunluk + sırayla eşit elemanlar
//   map    : aynı anahtarlar + eşit değerler; EKLEME SIRASI önemsiz
//   struct : aynı ad + eşit alanlar
// Boş liste ile boş map eşittir ([] henüz anahtar almamış bir map olarak da yazılır).
static bool vec_is_map(const std::vector<Value>& v) {
    return !v.empty() && v[0].type() == Value::STRING && v[0].str_ref() == "__assoc__";
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
        // Hızlı yol: aynı sırada kurulmuş iki map'te anahtar aynı konumdadır.
        if (a[i] == b[i]) { if (!(a[i + 1] == b[i + 1])) return false; continue; }
        bool found = false;
        for (size_t j = 1; j + 1 < b.size(); j += 2)
            if (a[i] == b[j]) { if (!(a[i + 1] == b[j + 1])) return false; found = true; break; }
        if (!found) return false;
    }
    return true;
}
// Sayı ↔ sayı KESİN karşılaştırma: -1 / 0 / 1; NaN varsa unordered=true (hiçbir şeye eşit değil,
// hiçbir sıralama doğru değil — IEEE / Go). int↔double karışık: double'ın tam kısmı int64 aralığındaysa
// tamsayı olarak, kalan kesir ayrıca karşılaştırılır (Python modeli). ESKİ: karışık çift double'a
// çevriliyordu → 2^53 üstünde kesinlik kaybı: 9007199254740993 == 9007199254740992.0 → true (ölçüldü).
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
    int r;                                              // i ile d'nin karşılaştırması
    if (std::isnan(d)) { unordered = true; return 0; }
    if (d >= 9223372036854775808.0)       r = -1;      // d her int64'ten büyük (+Inf dahil)
    else if (d < -9223372036854775808.0)  r = 1;       // d her int64'ten küçük (-Inf dahil)
    else {
        const double t = std::trunc(d);
        const int64_t ti = (int64_t)t;                  // aralık içinde: tanımlı dönüşüm
        if (i != ti) r = i < ti ? -1 : 1;
        else { const double f = d - t; r = f > 0 ? -1 : f < 0 ? 1 : 0; }
    }
    return ai ? r : -r;
}

bool Value::operator==(const Value& o) const {
    bool a_num = val_is_number(type_), b_num = val_is_number(o.type_);
    if (a_num && b_num) {
        // int↔int: KESİN int64 karşılaştır. to_float() ile double'a çevirmek 2^53 üstü
        // ayrık int64'leri eşitler (9007199254740993==...992, INT64_MAX-1==INT64_MAX →
        // true) — hem dil-düzeyi == yanlışı (LOOK 1'de her iki motorda, differential-görünmez), hem
        // VM constant-pool dedup korupsiyonu (compiler.cpp dedup bu =='i kullanır → int
        // literalleri çakışıp bozulur). Karışık int/float da kesin (num_cmp).
        if (type_ == INT && o.type_ == INT) return int_val == o.int_val;
        bool un; int c = num_cmp(*this, o, un);
        return !un && c == 0;
    }
    if (type_ != o.type_) return false;                     // farklı tür → asla eşit
    switch (type_) {
        case STRING: return str_ref() == o.str_ref();
        case BOOL:   return bool_val == o.bool_val;
        case NONE:   return true;                           // null == null
        case ARRAY: {
            if (ptr_val == o.ptr_val) return true;          // aynı depolama (henüz ayrılmamış kopya)
            if (!ptr_val || !o.ptr_val) return false;
            return vec_content_equal(*vec_ptr(), *o.vec_ptr());
        }
        case STRUCT: {
            if (ptr_val == o.ptr_val) return true;
            if (!ptr_val || !o.ptr_val) return false;
            const auto& a = *vec_ptr(); const auto& b = *o.vec_ptr();
            if (a.size() != b.size() || a[0].str_ref() != b[0].str_ref()) return false;
            for (size_t i = 2; i < a.size(); ++i) if (!(a[i] == b[i])) return false;
            return true;
        }
        default:     return ptr_val == o.ptr_val;           // fn/channel/ws: kimlik (tutamaçlar)
    }
}
// Sıralama: yalnız sayı↔sayı ve string↔string. Karışık tür → hata (sessizce
// string'i 0'a coerce edip yanlış sonuç vermek yerine bug'ı yüzeye çıkar).
[[noreturn]] static void cmp_type_error() {
    throw std::runtime_error(
        "Incomparable types: '<'/'>' only for number↔number and string↔string — "
        "convert with int()/float()/string() before comparing different types");
}
// Sayısal sıralama: int↔int ve karışık int/float KESİN (num_cmp; 2^53 üstü ayrık değerler
// double'da eşitlenirdi). NaN ile her sıralama false (IEEE / Go).
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
// Shift miktarı [0,63] dışındaysa (1<<64, 1<<-1) UB — aralık dışı → 0.
// Sol kaydırmada unsigned kullanılır (signed-overflow UB'sini önler).
// Negatif kaydırma sayısı açık hata (Go: "negative shift amount"). Eskiden sessizce 0 dönüyordu.
[[noreturn]] static void negative_shift(int64_t n) {
    throw std::runtime_error("Negative shift count: " + std::to_string(n));
}
Value Value::shift_left(const Value& o)  const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0) negative_shift(n);
    if (n >= 64) return Value((int64_t)0);   // Go: tamsayı genişliği kadar ya da fazlası → 0
    return Value((int64_t)((uint64_t)a << n));
}
Value Value::shift_right(const Value& o) const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0) negative_shift(n);
    // Aritmetik kaydırma: 63'ten büyük kaydırma işaret bitiyle dolar (Go: -8 >> 70 == -1). Eskiden 0
    // dönüyordu — -8 >> 63 == -1 iken -8 >> 64 == 0 tutarsızdı.
    if (n >= 64) return Value((int64_t)(a < 0 ? -1 : 0));
    return Value((int64_t)(a >> n));
}

// ── LookChannel methods ───────────────────────────────────────────────────────

// Kanal işlemi watchdog'u — LOOK_CHANNEL_TIMEOUT_MS set edilirse (>0) send/recv
// bu süreyi aşınca sonsuza asılmak yerine yakalanabilir bir hata fırlatır. Böylece
// eşleşecek gönderici/alıcı yoksa (mantık hatası kaynaklı deadlock) worker ebediyen
// kilitlenmez; kurtulur. Unset/0 = sonsuz blok (Go unbuffered rendezvous varsayılanı).
// Tek noktada — VM (CHAN_SEND/RECV) bu metotları çağırır.
static long channel_timeout_ms() {
    static const long v = []() -> long {
        const char* e = std::getenv("LOOK_CHANNEL_TIMEOUT_MS");
        if (e && *e) { long n = std::atol(e); if (n > 0) return n; }
        return 0;
    }();
    return v;
}

void LookChannel::send_val(Value val) {
    std::unique_lock<std::mutex> lk(mtx);
    long to = channel_timeout_ms();
    auto has_room = [this]{ return closed || queue.size() < capacity; };
    if (to > 0) {
        if (!not_full.wait_for(lk, std::chrono::milliseconds(to), has_room))
            throw std::runtime_error("channel send timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
    } else {
        not_full.wait(lk, has_room);
    }
    if (closed) throw std::runtime_error("send on closed channel");
    // İZOLASYON (cache/parallel deep_clone disiplini ile aynı): channel parallel'in
    // cross-thread iletişim primitive'i. Value ARRAY/MAP ise shared_ptr<container>'ı
    // gönderenle PAYLAŞIR — gönderen send sonrası $arr'ı hâlâ tutar (LOOK by-reference),
    // alıcı da aynı vector'ü alır → gönderen+alıcı aynı vector'e eşzamanlı mutasyon =
    // heap race (cache'in ikizi; TSan push_back/_M_realloc_insert). deep_clone snapshot
    // → queue izole değer tutar, gönderen ve alıcı ayrışır. Tek-alıcılı (pop) olduğu için
    // send'de tek klon yeter (cache multi-reader olduğundan get'te de klonluyordu).
    queue.push(val.deep_clone());
    not_empty.notify_one();
    if (unbuffered) {
        // Rendezvous: alıcı BU öğeyi alana dek bloke ol (Go unbuffered channel).
        // recv_gen ile kendi öğemizin alındığını izleriz; yanlış uyandırmada
        // (başka gönderici) bekleme sürer.
        uint64_t gen = recv_gen;
        auto taken = [this, gen]{ return closed || recv_gen > gen; };
        if (to > 0) {
            if (!not_full.wait_for(lk, std::chrono::milliseconds(to), taken))
                throw std::runtime_error("channel send (rendezvous) timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
        } else {
            not_full.wait(lk, taken);
        }
    }
}

Value LookChannel::recv_val() {
    std::unique_lock<std::mutex> lk(mtx);
    long to = channel_timeout_ms();
    auto has_item = [this]{ return closed || !queue.empty(); };
    if (to > 0) {
        if (!not_empty.wait_for(lk, std::chrono::milliseconds(to), has_item))
            throw std::runtime_error("channel receive timeout (possible deadlock; LOOK_CHANNEL_TIMEOUT_MS)");
    } else {
        not_empty.wait(lk, has_item);
    }
    if (queue.empty()) return Value();  // closed + empty → null
    Value v = std::move(queue.front());
    queue.pop();
    ++recv_gen;                 // rendezvous: bekleyen göndericiyi serbest bırak
    not_full.notify_all();      // birden çok gönderici bekliyor olabilir
    return v;
}

void LookChannel::close_chan() {
    std::unique_lock<std::mutex> lk(mtx);
    closed = true;
    not_empty.notify_all();
    not_full.notify_all();
}

} // namespace look

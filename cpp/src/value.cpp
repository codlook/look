#include "look/value.h"
#include "look/int_overflow.h"
#include "look/struct_types.h"
#include <mutex>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace look {

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
        case FLOAT:  return (int64_t)float_val;
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
Value Value::operator+(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() + o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_add_ovf(a, b, &r)) return Value((double)a + (double)b);
    return Value(r);
}
Value Value::operator-(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() - o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_sub_ovf(a, b, &r)) return Value((double)a - (double)b);
    return Value(r);
}
Value Value::operator*(const Value& o) const {
    arith_check(*this); arith_check(o);
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() * o.to_float());
    int64_t a = to_int(), b = o.to_int(), r;
    if (i64_mul_ovf(a, b, &r)) return Value((double)a * (double)b);
    return Value(r);
}
Value Value::operator/(const Value& o) const {
    arith_check(*this); arith_check(o);
    double d = o.to_float();
    if (d == 0.0) throw std::runtime_error("Division by zero");  // caught and enriched by interpreter
    if (type_ == FLOAT || o.type_ == FLOAT) return Value(to_float() / d);
    int64_t i = o.to_int();
    int64_t self_i = to_int();  // ham int_val değil — STRING/BOOL de doğru çevrilsin ("6"/2=3)
    // INT64_MIN / -1 signed-overflow UB — float'a promote
    if (i == -1 && self_i == INT64_MIN) return Value(-(double)self_i);
    if (i != 0 && self_i % i == 0) return Value(self_i / i);
    return Value(to_float() / d);
}
Value Value::operator%(const Value& o) const {
    arith_check(*this); arith_check(o);
    int64_t i = o.to_int();
    if (i == 0) throw std::runtime_error("Modulo by zero");
    if (i == -1) return Value((int64_t)0);  // INT64_MIN % -1 UB; a % -1 daima 0
    return Value(to_int() % i);
}
Value Value::pow(const Value& o) const { return Value(std::pow(to_float(), o.to_float())); }
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
bool Value::operator==(const Value& o) const {
    bool a_num = val_is_number(type_), b_num = val_is_number(o.type_);
    if (a_num && b_num) {
        // int↔int: KESİN int64 karşılaştır. to_float() ile double'a çevirmek 2^53 üstü
        // ayrık int64'leri eşitler (9007199254740993==...992, INT64_MAX-1==INT64_MAX →
        // true) — hem dil-düzeyi == yanlışı (her iki motor, differential-görünmez), hem
        // VM constant-pool dedup korupsiyonu (compiler.cpp dedup bu =='i kullanır → int
        // literalleri çakışıp bozulur). Karışık int/float double kalır (kaçınılmaz).
        if (type_ == INT && o.type_ == INT) return int_val == o.int_val;
        return to_float() == o.to_float();
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
// Sayısal sıralama: int↔int KESİN int64 (2^53 üstü ayrık int64 double'da eşitlenir →
// yanlış sıralama, bkz operator==). Karışık int/float double.
bool Value::operator<(const Value& o)  const {
    if (type_ == INT && o.type_ == INT) return int_val < o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) return to_float() < o.to_float();
    if (type_ == STRING && o.type_ == STRING) return str_ref() < o.str_ref();
    cmp_type_error();
}
bool Value::operator<=(const Value& o) const {
    if (type_ == INT && o.type_ == INT) return int_val <= o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) return to_float() <= o.to_float();
    if (type_ == STRING && o.type_ == STRING) return str_ref() <= o.str_ref();
    cmp_type_error();
}
bool Value::operator>(const Value& o)  const {
    if (type_ == INT && o.type_ == INT) return int_val > o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) return to_float() > o.to_float();
    if (type_ == STRING && o.type_ == STRING) return str_ref() > o.str_ref();
    cmp_type_error();
}
bool Value::operator>=(const Value& o) const {
    if (type_ == INT && o.type_ == INT) return int_val >= o.int_val;
    if (val_is_number(type_) && val_is_number(o.type_)) return to_float() >= o.to_float();
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
Value Value::shift_left(const Value& o)  const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0 || n >= 64) return Value((int64_t)0);
    return Value((int64_t)((uint64_t)a << n));
}
Value Value::shift_right(const Value& o) const {
    int64_t a = to_int(), n = o.to_int();
    if (n < 0 || n >= 64) return Value((int64_t)0);
    return Value((int64_t)(a >> n));
}

// ── LookChannel methods ───────────────────────────────────────────────────────

// Kanal işlemi watchdog'u — LOOK_CHANNEL_TIMEOUT_MS set edilirse (>0) send/recv
// bu süreyi aşınca sonsuza asılmak yerine yakalanabilir bir hata fırlatır. Böylece
// eşleşecek gönderici/alıcı yoksa (mantık hatası kaynaklı deadlock) worker ebediyen
// kilitlenmez; kurtulur. Unset/0 = sonsuz blok (Go unbuffered rendezvous varsayılanı).
// Tek noktada — hem tree-walk interpreter hem bytecode VM (CHAN_SEND/RECV) bu
// metotları çağırdığı için iki yol da otomatik kapsanır.
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

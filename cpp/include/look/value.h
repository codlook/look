#pragma once
// Dilin değeri (Value) ve yanındaki çekirdek türler: kanal, modül tablosu, hata türleri.
// Ev sahibi sınıfı (RuntimeHost) look/runtime_host.h'ta.

#include "look/ast.h"
#include "look/format.h"   // look_format_double — dilin tek double formatlayıcısı
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace look { struct WebContext; }

namespace look {

// ── Forward declarations for Value members ────────────────────────────────────
struct LookChannel;    // defined after Value (queue<Value> needs complete type)
struct WsConnection;   // defined in websocket.h
struct SseConnection;  // defined in sse.h
struct Closure;        // defined in bytecode.h — BYTECODE_FN type

// Tamsayı taşması hatası (value.cpp) — aritmetik, tekli eksi, abs ve ** aynı mesajı verir.
[[noreturn]] void look_int_overflow(int64_t a, const char* op, int64_t b);
class Value;
// Açık dönüşüm int() / float() (TO_INT / TO_FLOAT): to_int/to_float gibi gevşek ("abc" → 0) ama SAYISAL
// metin aralık dışıysa hata (Go strconv ErrRange) — eskiden int("99999999999999999999") → 0 sessizce.
Value look_explicit_int(const Value& v);
Value look_explicit_float(const Value& v);

// İç içe değer derinliği (dizi/struct içinde dizi/struct). Değeri DOLAŞAN özyinelemeli işlemler
// (string'e çevirme, ==, klonlama) bu sınırı aşınca yakalanabilir hata verir — Python'un
// RecursionError'ı gibi (PHP json 512, .NET 64). ESKİ: sınır yoktu; ölçüldü — normal derlemede
// ~40–80k, ASan'da ~3–7k derinlikte C++ yığını taşıp SÜRECİN TAMAMI çöküyordu. 1000 en kötü durumun
// (240 iç içe callback içinden, ASan) altında. Değerin BIRAKILMASI sınırsızdır (ValueArray).
constexpr int LOOK_MAX_NESTING = 1000;
[[noreturn]] void look_nesting_error();
// Yalnız dizi/struct dallarında kurulur (skaler sıcak yol dokunulmaz).
struct NestingGuard {
    // fiber-ok: bu işlemler askıya girmez (kullanıcı kodu çağırmaz, G/Ç yok) — sayaç aynı eşzamanlı
    // dolaşımın içinde artıp azalır, dolaşım bitince yine 0.
    static int& depth() { thread_local int d = 0; return d; }
    NestingGuard() { if (++depth() > LOOK_MAX_NESTING) { --depth(); look_nesting_error(); } }
    ~NestingGuard() { --depth(); }
    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
};

// ── Value ─────────────────────────────────────────────────────────────────────

// Dizi/struct depolaması kurucuları — ValueArray Value'dan sonra tanımlı; sınıf gövdesi bunları kullanır.
inline std::shared_ptr<std::vector<Value>> look_new_array();
inline std::shared_ptr<std::vector<Value>> look_new_array(const std::vector<Value>& copy);

class Value {
public:
    // STRUCT (LOOK 2) sona eklendi → mevcut tiplerin numarası değişmez. Depolaması ARRAY ile
    // aynıdır (shared_ptr<vector<Value>>) ama düzeni farklı ve SABİTTİR:
    //   [0] = struct adı (STRING)   [1] = tanım (ARRAY, üçlüler [alan, varsayılan, tip])
    //   [2 + i] = i. alanın değeri (bildirim sırası)
    // Ayrı tip olduğu için dizi bekleyen her kod (array::, indeksleme, foreach) onu DİZİ
    // sanmaz; alan erişimi ad araması değil indekstir. Kurallar: look/struct_types.h.
    // Fonksiyon değeri tek türdür: BYTECODE_FN (VM closure'ı). LOOK 2 öncesindeki FUNCTION
    // (ağaç-yürüten yorumlayıcının closure'ı) yorumlayıcıyla birlikte kalktı.
    enum Type { INT, FLOAT, STRING, BOOL, ARRAY, CHANNEL, WEBSOCKET, SSE_CONN, BYTECODE_FN, NONE, STRUCT };
    static Value make_struct(std::shared_ptr<std::vector<Value>> slots) {
        Value v(std::move(slots)); v.type_ = STRUCT; return v;
    }
    // STRUCT'ın slot vektörü (as_array ile aynı işaretçi; ad niyeti belli etsin diye ayrı).
    std::shared_ptr<std::vector<Value>> as_struct() const { return std::static_pointer_cast<std::vector<Value>>(ptr_val); }
    // Sıcak yol için sahipliksiz erişim: shared_ptr kopyası (atomik sayaç artır/azalt) yok.
    // Yalnız bu Value yaşarken geçerlidir.
    std::vector<Value>* vec_ptr() const { return static_cast<std::vector<Value>*>(ptr_val.get()); }
    // LOOK 2 yazınca-kopyala: dizi/struct depolaması başka bir Value ile paylaşılıyorsa bu Value
    // kendi (üst düzey) kopyasını alır; tek sahipse hiçbir şey yapmaz. Elemanlar paylaşılmaya
    // devam eder — onlar da yazılacakları anda aynı kuralla ayrılır.
    void detach() {
        if ((type_ == ARRAY || type_ == STRUCT) && ptr_val && ptr_val.use_count() > 1)
            ptr_val = look_new_array(*vec_ptr());
    }

    // B5: skalerler union'da (8 byte), STRING pointer arkasında → sizeof 80→32,
    // skaler kopyada string ctor/dtor yok. String literal'leri constant pool'dan
    // LOAD_CONST ile paylaşılır (refcount-bump, kopya yok) → RAM + hız.
    Value()                                         : type_(NONE),     int_val(0)   {}
    explicit Value(int64_t i)                       : type_(INT),      int_val(i)   {}
    explicit Value(int i)                           : type_(INT),      int_val((int64_t)i) {}
    explicit Value(double d)                        : type_(FLOAT),    float_val(d) {}
    explicit Value(bool b)                          : type_(BOOL),     bool_val(b)  {}
    explicit Value(const std::string& s)            : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(s)) {}
    explicit Value(std::string&& s)                 : type_(STRING),   int_val(0), ptr_val(std::make_shared<std::string>(std::move(s))) {}
    explicit Value(std::shared_ptr<std::vector<Value>> a) : type_(ARRAY), int_val(0), ptr_val(std::move(a)) {}
    explicit Value(std::shared_ptr<LookChannel>        c) : type_(CHANNEL),   int_val(0), ptr_val(std::move(c)) {}
    explicit Value(std::shared_ptr<WsConnection>       w) : type_(WEBSOCKET), int_val(0), ptr_val(std::move(w)) {}
    explicit Value(std::shared_ptr<SseConnection>      s) : type_(SSE_CONN),  int_val(0), ptr_val(std::move(s)) {}
    explicit Value(std::shared_ptr<Closure>            c) : type_(BYTECODE_FN), int_val(0), ptr_val(std::move(c)) {}

    Type        type()      const { return type_; }
    int64_t     as_int()    const { return int_val; }
    double      as_float()  const { return float_val; }
    std::string as_string() const {
        return (type_ == STRING && ptr_val)
            ? *static_cast<const std::string*>(ptr_val.get()) : std::string();
    }
    bool        as_bool()   const { return bool_val; }
    // Tek shared_ptr<void>'dan tipli erişim — type_ hangi tipin aktif olduğunu garanti eder.
    std::shared_ptr<std::vector<Value>>  as_array()    const { return std::static_pointer_cast<std::vector<Value>>(ptr_val); }
    std::shared_ptr<LookChannel>         as_channel()    const { return std::static_pointer_cast<LookChannel>(ptr_val); }
    std::shared_ptr<WsConnection>        as_websocket()  const { return std::static_pointer_cast<WsConnection>(ptr_val); }
    std::shared_ptr<SseConnection>       as_sse()        const { return std::static_pointer_cast<SseConnection>(ptr_val); }
    std::shared_ptr<Closure>             as_bytecode_fn()const { return std::static_pointer_cast<Closure>(ptr_val); }

    std::string to_string() const;
    double      to_float()  const;
    int64_t     to_int()    const;
    bool        is_truthy() const;

    // ARRAY için recursive deep copy; döngüsel referans güvenli (visited set ile kırılır)
    Value deep_clone() const {
        std::unordered_set<const void*> visited;
        return deep_clone_impl(visited);
    }

    // BYTECODE_FN (Closure) transitif deep-clone hook'u. value.h Closure'ı GÖREMEZ
    // (bytecode.h bunu sonra include eder) → vm katmanı bir cloner kaydeder. Kayıtsızsa
    // eski davranış (shallow). 58 parallel: bir closure yakalanınca, deep_clone onu shallow
    // kopyalarsa closure-içi cell'ler parent'la PAYLAŞIMLI kalır → veri yarışı (np1). Cloner
    // closure'ı klonlayıp cell-capture'larını + iç closure'ları özyineli klonlar.
    using BcFnCloner = Value(*)(const Value&, std::unordered_set<const void*>&);
    static BcFnCloner& bc_fn_cloner() { static BcFnCloner h = nullptr; return h; }
    // Hook özyinelemesi için visited-paylaşımlı deep-clone (deep_clone_impl private).
    Value deep_clone_tracked(std::unordered_set<const void*>& visited) const { return deep_clone_impl(visited); }

    // THREAD-SINIRI klonu: deep_clone gibi AMA STRING dahil (append_in_place yerinde yazar).
    // YALNIZ thread-crossing site'larda (parallel/timer/ws/sse/channel/cache) çağrılır.
    Value clone_for_thread() const {
        std::unordered_set<const void*> visited;
        return clone_for_thread_impl(visited);
    }
    Value clone_for_thread_tracked(std::unordered_set<const void*>& visited) const {
        return clone_for_thread_impl(visited);
    }

private:
    Value deep_clone_impl(std::unordered_set<const void*>& visited) const {
        if (type_ == ARRAY && ptr_val) {
            NestingGuard g;
            auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
            if (visited.count(arr.get())) return Value(); // döngü kır — null döndür
            visited.insert(arr.get());
            auto v = look_new_array();
            v->reserve(arr->size());
            for (const auto& e : *arr) v->push_back(e.deep_clone_impl(visited));
            visited.erase(arr.get());
            return Value(v);
        }
        if (type_ == STRUCT && ptr_val) {
            // Alan değerleri klonlanır; [0] ad ve [1] tanım değişmez → paylaşılır.
            NestingGuard g;
            auto sv = std::static_pointer_cast<std::vector<Value>>(ptr_val);
            if (visited.count(sv.get())) return Value();
            visited.insert(sv.get());
            auto v = look_new_array();
            v->reserve(sv->size());
            for (size_t i = 0; i < sv->size(); ++i)
                v->push_back(i < 2 ? (*sv)[i] : (*sv)[i].deep_clone_impl(visited));
            visited.erase(sv.get());
            return make_struct(std::move(v));
        }
        if (type_ == BYTECODE_FN && ptr_val) {
            // Closure → transitif klonla (cloner kayıtlıysa). Yoksa shallow (eski).
            if (auto h = bc_fn_cloner()) return h(*this, visited);
        }
        return *this; // scalar, channel, ws, sse — shallow copy yeterli (paylaşım kasıtlı)
    }

    // Thread-sınırı klonu (yalnız clone_for_thread'den). deep_clone_impl'in kopyası + STRING
    // dalı. deep_clone SICAK YOLDA olduğu için ayrı tutuluyor.
    Value clone_for_thread_impl(std::unordered_set<const void*>& visited) const {
        switch (type_) {
            case INT: case FLOAT: case BOOL: case NONE:
            case CHANNEL: case WEBSOCKET: case SSE_CONN:
                return *this;   // değer tipleri + paylaşımı KASITLI handle'lar
            case STRING:
                // append_in_place YERİNDE mutasyon → paylaşılan capture'da yarış; izole et.
                // Sözlük işareti adresle tanınır: kopyalanırsa sözlük listeye dönerdi (değişmez de).
                if (is_assoc_marker()) return *this;
                return ptr_val ? Value(*static_cast<const std::string*>(ptr_val.get())) : *this;
            case ARRAY: {
                if (!ptr_val) return *this;
                NestingGuard g;
                auto arr = std::static_pointer_cast<std::vector<Value>>(ptr_val);
                if (visited.count(arr.get())) return Value();
                visited.insert(arr.get());
                auto v = look_new_array();
                v->reserve(arr->size());
                for (const auto& e : *arr) v->push_back(e.clone_for_thread_impl(visited));
                visited.erase(arr.get());
                return Value(v);
            }
            case STRUCT: {
                if (!ptr_val) return *this;
                NestingGuard g;
                auto sv = std::static_pointer_cast<std::vector<Value>>(ptr_val);
                if (visited.count(sv.get())) return Value();
                visited.insert(sv.get());
                auto v = look_new_array();
                v->reserve(sv->size());
                for (size_t i = 0; i < sv->size(); ++i)
                    v->push_back(i < 2 ? (*sv)[i] : (*sv)[i].clone_for_thread_impl(visited));
                visited.erase(sv.get());
                return make_struct(std::move(v));
            }
            case BYTECODE_FN:
                if (ptr_val) { if (auto h = bc_fn_cloner()) return h(*this, visited); }
                return *this;
        }
        return *this; // unreachable — tüm Type'lar kapsandı (default YOK: yeni tip -> -Wswitch)
    }
public:

    Value operator+(const Value& o) const;
    Value operator-(const Value& o) const;
    Value operator*(const Value& o) const;
    Value operator/(const Value& o) const;
    Value operator%(const Value& o) const;
    Value pow(const Value& o)       const;
    Value concat(const Value& o)    const;
    // B7: akümülatör string'ine yerinde ekleme (amortize O(1)). `$s .= x` / `$s=$s.x`
    // döngüsünde her adımda tüm string'i kopyalayan O(n²) davranışı O(n)'e indirir.
    // STRING inline saklandığı ve register'lar bağımsız olduğu için mutasyon güvenli.
    void append_in_place(const Value& o);

    bool operator==(const Value& o) const;
    bool operator<(const Value& o)  const;
    bool operator<=(const Value& o) const;
    bool operator>(const Value& o)  const;
    bool operator>=(const Value& o) const;
    int  spaceship(const Value& o)  const;

    Value bitwise_and(const Value& o) const;
    Value bitwise_or(const Value& o)  const;
    Value bitwise_xor(const Value& o) const;
    Value bitwise_not()               const;
    Value shift_left(const Value& o)  const;
    Value shift_right(const Value& o) const;

    // ── Sözlük (map) işareti ──────────────────────────────────────────────────
    // Sözlük = ["<işaret>", k0, v0, k1, v1, …]. İşaret İÇERİKLE değil ADRESLE tanınır: tek bir sabit
    // metne işaret eden, sayaçsız (boş kontrol bloğu) bir STRING. ESKİ: işaret "__assoc__" METNİYDİ —
    // ilk elemanı o metin olan her liste sözlük sayılıyordu; saldırganın JSON dizisi
    // ["__assoc__","role","admin"] uygulamada role=>admin sözlüğüne dönüşüyordu (ölçüldü). JSON/form/
    // sorgu/LOOK kodu her metni yeni nesne olarak yaratır → işaret taklit edilemez. Kopyası ücretsiz
    // (atomik sayaç yok); append_in_place sayaçsız metni yerinde DEĞİŞTİRMEZ (use_count()==0 → kopya).
    static const std::string& assoc_marker_text() { static const std::string s("__assoc__"); return s; }
    static Value assoc_marker() {
        Value v; v.type_ = STRING;
        v.ptr_val = std::shared_ptr<void>(std::shared_ptr<void>(), const_cast<std::string*>(&assoc_marker_text()));
        return v;
    }
    bool is_assoc_marker() const { return type_ == STRING && ptr_val.get() == &assoc_marker_text(); }

    // STRING içeriğine kopyasız erişim (yalnız type_==STRING; diğerlerinde boş).
    // İç metodlar (to_string/operatörler) str_val yerine bunu kullanır.
    const std::string& str_ref() const {
        static const std::string kEmpty;
        return (type_ == STRING && ptr_val)
            ? *static_cast<const std::string*>(ptr_val.get()) : kEmpty;
    }

private:
    // B5: Value depolama — skalerler (int/float/bool) union'da tek 8-byte payload;
    // STRING ve tüm referans tipleri ptr_val arkasında. sizeof(Value) 80→32:
    //   type_(4)+pad(4) + union(8) + shared_ptr(16). Skaler kopya trivial-ucuz
    //   (string ctor/dtor yok, null shared_ptr kopyası atomiksiz).
    Type type_ = NONE;
    union {
        int64_t int_val;
        double  float_val;
        bool    bool_val;
    };
    // STRING → shared_ptr<std::string>; ARRAY/CHANNEL/WS/SSE/BYTECODE_FN → ilgili tip
    std::shared_ptr<void> ptr_val;
};

// ── ValueArray — dizi/struct depolaması: özyinelemesiz bırakma ────────────────
// std::vector<Value>'dan TÜRER (as_array/vec_ptr değişmez); tek farkı yıkıcısı. CPython'un
// "trashcan"ı gibi bırakma derinliği izlenir; eşiği aşan depolamanın elemanları yıkılmaz, listeye
// taşınır ve en dıştaki bırakmada döngüyle yıkılır → yığın kullanımı derinlikten bağımsız.
// ESKİ: iç içe dizi/struct son sahibinden düşünce vector→Value→vector zinciri C++ yığınını
// taşırıyordu (ölçüldü: ~200k derinlikte segfault; tek istek bütün süreci düşürüyordu).
// Kanca ~Value'da DEĞİL kapsayıcıda: ~Value'da her eleman yıkımına binerdi (ölçüldü: istek başına
// +%4,7 komut); burada depolama başına bir kez. Dizi depolaması yalnız bununla kurulur —
// tests/value_release_test.sh düz std::vector<Value> kuran kaynak satırını reddeder.
void look_release_values(std::vector<Value>& v) noexcept;   // value.cpp

// Büyük sözlüğün anahtar dizini (look/assoc_index.h). Depolama yine düz liste; dizin yalnız
// arama hızlandırıcısıdır ve kopyalanmaz (kopya ilk aramada kendi dizinini kurar).
struct AssocIndex;
void look_assoc_index_free(AssocIndex* ix) noexcept;   // value.cpp

struct ValueArray : std::vector<Value> {
    using std::vector<Value>::vector;
    using std::vector<Value>::operator=;
    ValueArray() = default;
    ValueArray(const ValueArray& o) : std::vector<Value>(o) {}
    ValueArray(ValueArray&& o) noexcept : std::vector<Value>(std::move(o)) {}
    ValueArray& operator=(const ValueArray& o) { drop_index(); std::vector<Value>::operator=(o); return *this; }
    ValueArray& operator=(ValueArray&& o) noexcept { drop_index(); std::vector<Value>::operator=(std::move(o)); return *this; }
    ValueArray& operator=(const std::vector<Value>& o) { drop_index(); std::vector<Value>::operator=(o); return *this; }
    ValueArray& operator=(std::vector<Value>&& o) noexcept { drop_index(); std::vector<Value>::operator=(std::move(o)); return *this; }
    ValueArray(const std::vector<Value>& o) : std::vector<Value>(o) {}
    ValueArray(std::vector<Value>&& o) noexcept : std::vector<Value>(std::move(o)) {}
    ~ValueArray() { drop_index(); if (!empty()) look_release_values(*this); }

    // Okuma yolu dizini tembel kurar (yalnız boştan → kurulu, CAS); birden çok iş parçacığı aynı
    // paylaşılan sözlüğü okuyabilir. Dizini DEĞİŞTİREN yazma yolu depoyu tek sahipli tutar (COW).
    std::atomic<AssocIndex*> assoc_idx{nullptr};
    void drop_index() noexcept {
        if (AssocIndex* ix = assoc_idx.exchange(nullptr, std::memory_order_acq_rel)) look_assoc_index_free(ix);
    }
};

inline std::shared_ptr<std::vector<Value>> look_new_array() { return std::make_shared<ValueArray>(); }
inline std::shared_ptr<std::vector<Value>> look_new_array(const std::vector<Value>& copy) {
    return std::make_shared<ValueArray>(copy);
}

// ── LookChannel — Go-style channel (defined after Value — queue<Value> needs complete type)
struct LookChannel : std::enable_shared_from_this<LookChannel> {
    std::queue<Value>       queue;
    std::mutex              mtx;
    std::condition_variable not_empty;  // receivers wait
    std::condition_variable not_full;   // senders wait when full
    size_t                  capacity;
    bool                    closed = false;
    bool                    unbuffered = false;  // channel(0): Go rendezvous
    uint64_t                recv_gen = 0;         // her recv'de artar (rendezvous)

    // channel(0) = unbuffered/senkron (Go semantiği): gönderici alıcı öğeyi
    // alana dek bloke olur. Eskiden 0 = SINIRSIZ buffer'dı → backpressure yok,
    // hızlı üretici + yavaş tüketici = OOM. Artık kapasite 1 + rendezvous.
    explicit LookChannel(size_t cap = 128)
        : capacity(cap == 0 ? 1 : cap), unbuffered(cap == 0) {}

    // phase: parallel() görevinin betik-sonu durumu (parallel_runtime.h TaskExit::phase). Görev
    // beklerken IN_EFFECT'tir; değer TESLİM edilmeden hemen önce RUNNING'e alınır (tamponsuzda
    // alıcı aldığı anda) — teslimden sonra yalnız dönüşü kalan görev "bitmiş" sayılabilsin.
    void  send_val(Value val, std::atomic<int>* phase = nullptr);
    std::atomic<int>*       rv_phase = nullptr;   // tamponsuz: bekleyen göndericinin durumu
    Value recv_val();
    void  close_chan();
    bool  is_closed() { std::unique_lock<std::mutex> lk(mtx); return closed; }
    int   sz()        { std::unique_lock<std::mutex> lk(mtx); return (int)queue.size(); }
};

// channel([size]) — TEK tanım (komut satırı + web): boyutsuz 128, 0 = tamponsuz (Go), N > 0 = N yer,
// negatif ya da tamsayı olmayan = hata. Eskiden 0 komut satırında SINIRSIZ, web'de 1 048 576 yerdi
// (belgedeki "tamponsuz" hiçbir yoldan kurulamıyordu; yavaş alıcıda bellek sınırsız büyüyordu).
std::shared_ptr<LookChannel> make_channel(const std::vector<Value>& args);

// ── VM geri çağırma köprüsü ───────────────────────────────────────────────────
// Higher-order builtin'ler (array::map/filter/reduce…) callback'i ev sahibinin invoke'uyla
// çağırır; callback bir BYTECODE_FN (VM closure) olur. Etkin VM kendini thread-local kaydeder;
// invoke buraya delege eder. Kayıtlı VM yoksa available()==false.
bool  vm_bridge_available();
Value vm_bridge_invoke(const Value& fn, std::vector<Value>& args);
// VM (vm.cpp) çalışırken kendi closure-invoker hook'unu kaydeder.
void  register_vm_bridge(Value (*hook)(const Value& fn, std::vector<Value>& args));

// ── Module ────────────────────────────────────────────────────────────────────

using NativeFn = std::function<Value(std::vector<Value>)>;

struct Module {
    std::string name;
    std::map<std::string, NativeFn> functions;
};

// ── Servis kaydı (app::set / app::get) ────────────────────────────────────────
// Setup'ta yazılan paylaşılan servisleri (db, cache, config…) tutar. Tüm
// dispatch kopyaları shared_ptr ile AYNI registry'yi paylaşır (make_dispatch_copy).
// Mutex: setup tek thread yazar, dispatch çok thread okur; nadir request-içi
// yazma da güvenli. Kilit maliyeti route başına 1 kez, DB I/O yanında ihmal.
struct ServiceRegistry {
    std::mutex                   mtx;
    std::map<std::string, Value> items;
};

// ── Error system ──────────────────────────────────────────────────────────────

struct StackFrame {
    std::string function;  // function/closure name, empty = top-level
    int         line = 0;  // çağrının yapıldığı satır; -1 = "… N more calls" ara satırı (function metni)
    std::string file;      // çağrı yerinin dosyası, "" = ana betik (dahil edilen dosyada adı yazılır)
};

class LookRuntimeError : public std::exception {
public:
    std::string              message;
    SourceLocation           location;
    std::vector<StackFrame>  stack;
    Value                    value;   // set by error::new() — catch e gets this Value
    bool                     has_value = false;

    LookRuntimeError(std::string msg,
                     SourceLocation loc = {},
                     std::vector<StackFrame> stk = {})
        : message(std::move(msg)), location(std::move(loc)), stack(std::move(stk)) {}

    LookRuntimeError(Value v, SourceLocation loc = {}, std::vector<StackFrame> stk = {})
        : message(v.to_string()), location(std::move(loc)), stack(std::move(stk))
        , value(std::move(v)), has_value(true) {}

    const char* what() const noexcept override { return message.c_str(); }

    std::string format() const {
        std::string out = "\nRuntime Error: " + message + "\n";
        if (location.line > 0) {
            if (!location.file.empty())
                out += "  File: " + location.file + "\n";
            out += "  Line: " + std::to_string(location.line);
            if (location.column > 0)
                out += ", Column: " + std::to_string(location.column);
            out += "\n";
        }
        if (!stack.empty()) {
            out += "Stack trace:\n";
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (it->line < 0) { out += "  " + it->function + "\n"; continue; }
                out += "  at " + (it->function.empty() ? "<main>" : it->function);
                if (it->line > 0) {
                    out += " (line " + std::to_string(it->line);
                    if (!it->file.empty()) out += " in " + it->file;
                    out += ")";
                }
                out += "\n";
            }
        }
        return out;
    }
};

class LookParseError : public std::exception {
public:
    std::string  message;
    int          line   = 0;
    int          column = 0;
    std::string  file;

    LookParseError(std::string msg, int ln = 0, int col = 0, std::string f = "")
        : message(std::move(msg)), line(ln), column(col), file(std::move(f)) {}

    const char* what() const noexcept override { return message.c_str(); }

    std::string format() const {
        std::string out = "\nParse Error: " + message + "\n";
        if (!file.empty()) out += "  File: " + file + "\n";
        if (line > 0) {
            out += "  Line: " + std::to_string(line);
            if (column > 0) out += ", Column: " + std::to_string(column);
            out += "\n";
        }
        return out;
    }
};

// exit(code) — script'i tamamen durdurur, FastCGI döngüsünü bozmaz
class ExitException : public std::exception {
public:
    explicit ExitException(int code = 0) : code_(code) {}
    const char* what() const noexcept override { return "exit"; }
    int code() const { return code_; }
private:
    int code_;
};

// ── StructFieldDef ────────────────────────────────────────────────────────────

struct StructFieldDef {
    std::string name;
    std::string type;                // LOOK 2: "" = tipsiz
    bool        has_default = false;
    Value       default_val;
};

} // namespace look

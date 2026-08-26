// LOOK Bytecode VM
//
// Dispatch döngüsü: switch(opcode) → GCC/Clang jump table.
// Value tipi interpreter.h ile aynı — tam uyumluluk.
// Struct convention: ARRAY + __struct__ sentinel (interpreter ile aynı).

#include <atomic>
#include <climits>
#include "look/vm.h"
#include "look/bytecode.h"
#include "look/web.h"
#include "look/interpreter.h"   // ExitException — CALL_BUILTIN (interpreter NativeFn) fırlatabilir
#include "look/parallel_runtime.h"
#include "look/logger.h"
#include "look/builtins.h"      // builtin_names() — hata mesajinda modul/fonksiyon adi
#include "look/int_overflow.h"  // i64_add_ovf — INT+INT aritmetik fast-path

#include <sstream>
#include <cmath>
#include <cassert>
#include <thread>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>

namespace {
// LOOK_WARN_UNDEF=1 → tanımsız değişken okumalarını logla (davranışı DEĞİŞTİRMEZ).
// Bir kez okunur; LOAD_GLOBAL'in yalnız ıska dalında bakılır → sıcak yola maliyeti yok.
const bool g_warn_undef = [] {
    const char* e = std::getenv("LOOK_WARN_UNDEF");
    return e && e[0] == '1';
}();
} // namespace

namespace look {

// 58 parallel transitif deep-clone hook'u. interpreter.h Closure'ı göremez (bytecode.h
// onu sonra tanımlar) → deep_clone_impl BYTECODE_FN görünce burada kayıtlı cloner'ı çağırır.
// Bir Closure klonlanırken cell-capture'ları VE iç closure'ları özyineli klonlanır; aksi
// halde parallel task'ın yakaladığı closure'ın içindeki cell parent'la paylaşımlı kalır
// (np1 veri yarışı). Channel/scalar capture'lar paylaşımlı kalır (deep_clone_impl *this).
static Value clone_bytecode_fn(const Value& v, std::unordered_set<const void*>& visited) {
    auto src = v.as_bytecode_fn();
    if (!src) return v;
    if (visited.count(src.get())) return Value(); // döngü kır
    visited.insert(src.get());
    auto nc = std::make_shared<Closure>(src->proto);
    nc->captures = src->captures;                  // sığ; aşağıda cell/closure olanlar derinleşir
    // R-02: parallel task'a verilen closure'un capture izolasyonu — İZİN LİSTESİ
    // (fail-open) yerine YASAK LİSTESİ (fail-safe). Paylaşımı KASITLI olan handle tipleri
    // (channel/ws/sse — cross-thread iletişim primitifleri) DIŞINDA her capture deep_clone
    // edilir. Böylece `use ($dizi)` gibi düz ARRAY capture'ı da izole olur (eski hâl yalnız
    // cell + iç-closure klonluyordu; düz dizi sığ shared_ptr'la PAYLAŞILIP parallel task'lar
    // aynı vector'ü eşzamanlı mutasyona uğratıp YARIŞIYORDU — t1, TSan). Kritik: Value'ya
    // yeni bir tip eklendiğinde otomatik GÜVENLİ tarafta başlar — izin listesi olsaydı her
    // yeni tip sessiz-paylaşım bug'ı olurdu (bu sınıfın 3. yaması). deep_clone_impl her tipi
    // doğru ele alır: ARRAY özyineli klon, closure hook'la klon, scalar/string *this (ucuz,
    // immutable). Interpreter'ın zaten yaptığı izolasyonla parite (differential-güvenli).
    // (cell flag'i artık gereksiz — cell'ler de handle olmadıkça klonlanır.)
    for (size_t j = 0; j < nc->captures.size(); ++j) {
        Value::Type t = nc->captures[j].type();
        bool shared_by_design = (t == Value::CHANNEL || t == Value::WEBSOCKET
                              || t == Value::SSE_CONN);
        if (!shared_by_design)
            nc->captures[j] = nc->captures[j].deep_clone_tracked(visited);
    }
    visited.erase(src.get());
    return Value(nc);
}
static const bool s_bc_cloner_registered = [] {
    Value::bc_fn_cloner() = &clone_bytecode_fn;
    return true;
}();

// ── VM/interpreter callback köprüsü (hook tarafı) ─────────────────────────────
// Bridge fonksiyonları interpreter.cpp'de (hem CLI hem fcgi linkler); VM burada
// yalnızca hook'unu kaydeder. Aktif VM run() içinde thread-local set edilir.
static thread_local VM* t_active_vm = nullptr;

static Value vm_bridge_hook(const Value& fn, std::vector<Value>& args) {
    if (!t_active_vm)
        throw LookVmError("vm_bridge_hook: no active VM");
    if (fn.type() != Value::BYTECODE_FN)
        throw LookVmError("vm_bridge_hook: BYTECODE_FN expected");
    return t_active_vm->call_closure(*fn.as_bytecode_fn(), args);
}

// ── VM ctor ───────────────────────────────────────────────────────────────────

VM::VM(SharedState shared, std::ostream& output)
    : shared_(shared), output_(output)
{
    regs_.reserve(1024);
    call_stack_.reserve(64);
}

void VM::set_globals(std::unordered_map<std::string, Value> g) {
    globals_ = std::move(g);
    global_cache_.clear();   // A2: globals_ tümden değişti → önbelleklenen Value*'lar geçersiz
}
void VM::set_web_context(WebContext* ctx)                        { web_ctx_ = ctx; }
void VM::set_ws_connection(std::shared_ptr<WsConnection> ws)    { ws_conn_ = ws; }
void VM::set_sse_connection(std::shared_ptr<SseConnection> sse) { sse_conn_ = sse; }

// ── execute ───────────────────────────────────────────────────────────────────

void VM::execute(const CompiledProgram& prog) {
    auto cl = std::make_shared<Closure>(prog.main_proto);
    push_frame(cl.get(), prog.main_proto->reg_count, -1);
    run();
}

// ── call_closure ──────────────────────────────────────────────────────────────

Value VM::call_closure(const Closure& closure, std::vector<Value> args) {
    if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
        throw LookVmError("Stack overflow (max " + std::to_string(MAX_CALL_DEPTH) + ")");

    auto* proto = closure.proto.get();
    int base = push_frame(&closure, proto->reg_count, -1);

    // Argümanları register'lara yaz
    int argc = (int)args.size();
    call_stack_.back().argc = argc;   // varsayılan param prologue'u için
    if (!proto->variadic) {
        for (int i = 0; i < proto->arity && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
    } else {
        int fixed = proto->arity - 1;
        for (int i = 0; i < fixed && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
        // Variadic: kalan args → ARRAY
        auto varr = std::make_shared<std::vector<Value>>();
        for (int i = fixed; i < argc; ++i) varr->push_back(std::move(args[i]));
        if (fixed >= 0 && proto->arity > 0)
            regs_[base + fixed] = Value(varr);
    }

    return run();
}

// ── push_frame ────────────────────────────────────────────────────────────────

int VM::push_frame(const Closure* cl, int reg_count, int ret_reg) {
    int base = (int)regs_.size();
    regs_.resize(base + reg_count);
    call_stack_.push_back({cl->proto.get(), cl, 0, base, ret_reg});
    return base;
}

// ── dispatch_routes ───────────────────────────────────────────────────────────

void VM::dispatch_routes(const std::string& method, const std::string& path) {
    if (!shared_.routes) return;
    for (auto& entry : *shared_.routes) {
        size_t colon = entry.pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = entry.pattern.substr(0, colon);
        std::string p = entry.pattern.substr(colon + 1);
        if (m != method && m != "*") continue;
        std::vector<Value> params;
        if (route_match(p, path, params)) {
            last_matched_route_ = entry.app_index;
            // ── TEST-ONLY fault injection (env-gated, prod-etkisiz) ──────────
            // LOOK_VM_FORCE_FAIL=<substr>: eşleşen route pattern'i <substr>
            // içeriyorsa VM yürütmesini kasıtlı fail ettir → http_main
            // interpreter-fallback yolu (ve route-pinning) tetiklenir. Bu YALNIZ
            // fallback-doğruluk CI guard'ı içindir. Env yoksa: statik pointer
            // NULL → hot-path'te tek pointer testi, davranış DEĞİŞMEZ.
            {
                static const char* const s_force_fail =
                    std::getenv("LOOK_VM_FORCE_FAIL");
                if (s_force_fail && *s_force_fail &&
                    entry.pattern.find(s_force_fail) != std::string::npos)
                    throw std::runtime_error(
                        std::string("LOOK_VM_FORCE_FAIL test-hook: ") + entry.pattern);
            }
            // Route daha önce VM'de hata verdiyse kalıcı interpreter'a sabitli
            // route_disabled byte-flag'i başka request-thread'i (http_main VM-fallback yolu)
            // yazabilir → atomic_ref ile oku (data race/UB önle; relaxed — advisory flag).
            if (shared_.route_disabled && entry.app_index >= 0 &&
                entry.app_index < (int)shared_.route_disabled->size() &&
                std::atomic_ref<uint8_t>(const_cast<uint8_t&>((*shared_.route_disabled)[entry.app_index])).load(std::memory_order_relaxed))
                throw VmRouteDisabled();
            // Route-level middleware'leri çalıştır
            bool stopped = false;
            for (auto* mw : entry.middlewares) {
                try { call_closure(*mw, {}); }
                catch (const RouteStopException&) { stopped = true; break; }
            }
            if (!stopped)
                call_closure(*entry.fn, std::move(params));
            return;
        }
    }
    // 404 route ara
    for (auto& entry : *shared_.routes) {
        size_t colon = entry.pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = entry.pattern.substr(0, colon);
        if (m == "404") {
            call_closure(*entry.fn, {});
            return;
        }
    }
    if (web_ctx_) { web_ctx_->status_code = 404; web_ctx_->status_text = "Not Found"; }
    output_ << "404 Not Found";
}

bool VM::route_match(const std::string& pattern, const std::string& path,
                     std::vector<Value>& params) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream ss(s);
        std::string t;
        while (std::getline(ss, t, '/')) if (!t.empty()) v.push_back(t);
        return v;
    };
    auto pp = split(pattern), rp = split(path);
    if (pp.size() != rp.size()) return false;
    // İsimli param'ları topla; yalnız TAM eşleşmede web_ctx_'e yaz (kısmi
    // eşleşme route_params'ı kirletmesin).
    std::vector<std::pair<std::string, std::string>> named;
    for (size_t i = 0; i < pp.size(); ++i) {
        if (pp[i].size() > 2 && pp[i].front() == '{' && pp[i].back() == '}') {
            params.push_back(Value(rp[i]));
            named.emplace_back(pp[i].substr(1, pp[i].size() - 2), rp[i]);
        } else if (pp[i] != rp[i]) return false;
    }
    // request::param("id") / route::param("id") VM yolunda da çalışsın
    // (interpreter route_params'ı dolduruyor — divergence giderildi).
    if (web_ctx_)
        for (auto& [k, v] : named) web_ctx_->route_params[k] = v;
    return true;
}

// ── Value helpers ─────────────────────────────────────────────────────────────

// "12" / "-3" gibi TAM SAYI metni mi? Dizi anahtari sayisal indeks mi yoksa
// assoc anahtari mi diye ayirmak icin. (interpreter.cpp'de ayni kural var.)
static bool look_is_int_key(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool VM::val_truthy(const Value& v) { return v.is_truthy(); }

std::string VM::val_to_str(const Value& v) { return v.to_string(); }

Value VM::array_get(const Value& arr, const Value& key) {
    if (arr.type() != Value::ARRAY) return Value();
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, k1, v1, ...] — sentinel at position 0 (interpreter convention)
    // str_ref(): B5'te string pointer arkasında — as_string()/to_string() her erişimde
    // kopya döndürür. Sentinel ve key karşılaştırmaları kopyasız (nesne-lookup sıcak yolu).
    if (!vec.empty() && vec[0].type() == Value::STRING && vec[0].str_ref() == "__assoc__") {
        // Lookup-key kopyasız: STRING anahtar (sıcak yol, $row["name"]) doğrudan
        // str_ref(); sayısal anahtar ($a[5]) yine "5"e coerce edilir (semantik korunur).
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref()
                                                             : (ktmp = key.to_string());
        // Also handle __struct__ tag: ["__assoc__", "__struct__", "Name", k, v, ...]
        size_t start = 1;
        if (vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__")
            start = 3;
        for (size_t i = start; i + 1 < vec.size(); i += 2) {
            if (vec[i].type() == Value::STRING ? vec[i].str_ref() == k : vec[i].to_string() == k)
                return vec[i+1];
        }
        return Value();
    }
    // Liste + tam-sayi-olmayan string anahtar: boyle bir anahtar yok (assoc'ta
    // bulunamayan anahtarla ayni sozlesme). $obj.alan erisimi de buraya duser.
    if (key.type() == Value::STRING && !look_is_int_key(key.str_ref())) return Value();

    // Sayisal indeks — TREE-WALK REFERANS SEMANTIGI (interpreter.cpp ile ayni):
    // negatif = sondan, aralik disi = HATA.
    // ESKI HATA: VM aralik disinda ve negatifte SESSIZCE null donuyordu, oysa
    // interpreter hata firlatiyor/sondan sayiyordu — ayni program iki motorda iki
    // farkli sonuc veriyordu:
    //   $a=[1,2,3]; $a[99]  ->  VM null   / tree-walk HATA
    //   $a=[1,2,3]; $a[-1]  ->  VM null   / tree-walk 3 (sondan)
    // Sessiz null, yanlis veriyi gizleyen sinifin ta kendisi (tanimsiz degisken
    // STRICT karariyla ayni gerekce): dongude siniri asan erisim hata vermeli.
    int64_t i = key.to_int();
    if (i < 0) i += (int64_t)vec.size();
    if (i < 0 || i >= (int64_t)vec.size())
        throw LookVmError("Array index " + std::to_string(key.to_int()) + " out of bounds");
    return vec[(size_t)i];
}

void VM::array_set(Value& arr, const Value& key, const Value& val) {
    if (arr.type() == Value::NONE) {
        arr = Value(std::make_shared<std::vector<Value>>());
    }
    if (arr.type() != Value::ARRAY) return;
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, ...] — sentinel at position 0 (interpreter convention)
    bool is_assoc = !vec.empty() && vec[0].type() == Value::STRING && vec[0].str_ref() == "__assoc__";
    // "12"/"-3" gibi tam sayi METNI sayisal indekstir, assoc anahtari degil
    // (interpreter bu metinleri hep sayisal indeks sayiyordu — motorlar ayrisiyordu).
    bool str_key = key.type() == Value::STRING && !look_is_int_key(key.str_ref());
    if (is_assoc || str_key) {
        // Lookup-key kopyasız (array_get ile aynı): STRING anahtar str_ref();
        // sayısal anahtar "5"e coerce. Yeni giriş Value(k) yine kopyalar.
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref()
                                                             : (ktmp = key.to_string());
        // ESKI HATA: liste henuz assoc DEGILKEN asagidaki dongu onu cift listesi
        // sanip tariyor (start=0), sonra basina yalnizca sentinel ekliyordu:
        //   $a=[1,2,3]; $a["k"]="X"  ->  ["__assoc__",1,2,3,"k","X"]
        // cift olarak okununca 1->2, 3->"k" cikiyor: TUM VERI YOK OLUYOR ve yeni
        // deger de erisilemiyor. Once elemanlari SAYISAL INDEKSLERIYLE anahtarlayarak
        // donustur (array::set ile ayni sozlesme).
        if (!is_assoc) {
            std::vector<Value> conv;
            conv.reserve(vec.size() * 2 + 1);
            conv.push_back(Value(std::string("__assoc__")));
            for (size_t i = 0; i < vec.size(); ++i) {
                conv.push_back(Value(std::to_string(i)));
                conv.push_back(vec[i]);
            }
            vec.swap(conv);
        }
        // Find existing (skip sentinel at [0] and optional struct tags)
        size_t start = 1;
        if (vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].str_ref() == "__struct__")
            start = 3;
        for (size_t i = start; i + 1 < vec.size(); i += 2) {
            if (vec[i].type() == Value::STRING ? vec[i].str_ref() == k : vec[i].to_string() == k)
                { vec[i+1] = val; return; }
        }
        // New entry — append key, value at end. When the key is already a STRING Value,
        // store it directly (shared_ptr copy) instead of re-materializing it with a fresh
        // make_shared<string> — the key literal already lives in the constant pool, so
        // Value(k) was allocating a duplicate of a constant on every insert (5 per assoc in
        // object_create). Only a coerced numeric key ("5") needs a freshly built string.
        if (key.type() == Value::STRING) vec.push_back(key);
        else                             vec.push_back(Value(k));
        vec.push_back(val);
        return;
    }
    // Sayisal indeks — TREE-WALK REFERANS SEMANTIGI (interpreter.cpp ile ayni):
    // negatif = sondan, size'a esit = sona ekleme, digerleri HATA.
    // ESKI HATA: VM aralik disinda ve negatifte YAZMAYI SESSIZCE ATLIYORDU —
    // atama hicbir sey yapmiyor, hata da vermiyordu:
    //   $a=[1,2,3]; $a[99]="X"  ->  VM [1,2,3] (atama kayboldu) / tree-walk HATA
    //   $a=[1,2,3]; $a[-1]="X"  ->  VM [1,2,3] (atama kayboldu) / tree-walk [1,2,"X"]
    // "Yazdim ama yazilmadi" en sinsi sinif: veri kaybi, hata yok.
    int64_t idx = key.to_int();
    if (idx < 0) idx += (int64_t)vec.size();
    if (idx == (int64_t)vec.size()) { vec.push_back(val); return; }
    if (idx < 0 || idx > (int64_t)vec.size())
        throw LookVmError("Array index " + std::to_string(key.to_int()) + " out of bounds");
    vec[(size_t)idx] = val;
}

Value VM::get_field(const Value& obj, const std::string& field) {
    return array_get(obj, Value(field));
}

void VM::set_field(Value& obj, const std::string& field, const Value& val) {
    array_set(obj, Value(field), val);
}

// ── run — dispatch döngüsü ────────────────────────────────────────────────────

Value VM::run() {
    // Callback köprüsü: bu VM'i aktif olarak kaydet (RAII ile geri al). Nested
    // run() aynı VM'e set eder — zararsız. array::map gibi builtin'ler callback'i
    // interpreter->invoke → vm_bridge_invoke → bu VM'in call_closure'ıyla çalıştırır.
    VM* _prev_active = t_active_vm;
    t_active_vm = this;
    register_vm_bridge(&vm_bridge_hook);   // thread-local hook'u kaydet (idempotent)
    struct ActiveGuard { VM* prev; ~ActiveGuard() { t_active_vm = prev; } } _ag{_prev_active};

    // Try tabanı: bu run() yalnız kendi push ettiği handler'ları kullanır. İç içe
    // run() (köprüden gelen closure çağrısı) dış handler'a atlayamaz → LookVmThrow ile
    // C++ sınırından propagate eder. Çıkışta taban ve artık handler'lar geri alınır.
    struct FloorGuard {
        VM* vm; size_t prev_floor; size_t entry_size;
        ~FloorGuard() {
            if (vm->try_stack_.size() > entry_size) vm->try_stack_.resize(entry_size);
            vm->try_floor_ = prev_floor;
        }
    } _fg{this, try_floor_, try_stack_.size()};
    try_floor_ = try_stack_.size();

call_dispatch:
    while (!call_stack_.empty()) {
        Frame& frame = call_stack_.back();
        const FunctionProto* proto = frame.proto;
        int base = frame.base;

        // A2: bu proto'nun global inline cache vektörü (frame girişinde bir kez — frame'ler
        // instruction'lardan çok daha seyrek değişir). unordered_map rehash mapped-value
        // referansını geçersiz kılmaz → gcache pointer'ı frame boyunca stabildir.
        std::vector<Value*>* gcache = &global_cache_[proto];
        if (gcache->size() != proto->constants.size())
            gcache->assign(proto->constants.size(), nullptr);

        try {
        while (frame.ip < (int)proto->code.size()) {
            const Instruction& ins = proto->code[frame.ip++];

// A1: Sıcak yol register erişimi. Compiler register indekslerini derleme-zamanı
// garanti ettiği için release'de bounds-check ölü maliyet → operator[]. Debug/ASan
// build'de .at() korunur (bytecode üretimi bug'ı taşarsa yakalanır, sessiz UB olmaz).
#ifdef NDEBUG
#define R(x) regs_[(size_t)(base + (x))]
#else
#define R(x) regs_.at((size_t)(base + (x)))
#endif
#define CONST(i) proto->constants[i]

            switch (ins.op) {

            // ── Load ──────────────────────────────────────────────────────────
            case OpCode::LOAD_CONST:
                R(ins.a) = CONST(ins.b);
                break;
            case OpCode::LOAD_CONST_W:
                R(ins.a) = CONST((uint16_t(ins.b)<<8)|ins.c);
                break;
            case OpCode::LOAD_NULL:  R(ins.a) = Value();        break;
            case OpCode::LOAD_TRUE:  R(ins.a) = Value(true);    break;
            case OpCode::LOAD_FALSE: R(ins.a) = Value(false);   break;
            case OpCode::LOAD_INT:   R(ins.a) = Value((int)(int8_t)ins.b); break;
            case OpCode::MOVE:       R(ins.a) = R(ins.b);       break;
            case OpCode::LOAD_VAR:   R(ins.a) = R(ins.b);       break;
            case OpCode::STORE_VAR:  R(ins.a) = R(ins.b);       break;

            case OpCode::LOAD_GLOBAL: {
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                // A2 inline cache: bu ni için çivilenmiş slot varsa doğrudan oku (register hızı).
                if (Value* hit = (*gcache)[ni]) { R(ins.a) = *hit; break; }
                // str_ref(): as_string() kopyasını önle (B5'te string pointer arkasında;
                // her global erişimi bir string kopyası ödemeliydi). find const& alır.
                const std::string& gname = CONST(ni).str_ref();
                auto it = globals_.find(gname);
                if (it != globals_.end()) {
                    (*gcache)[ni] = &it->second;   // slot'u çivile — sonraki erişimler ıskasız
                    R(ins.a) = it->second;
                } else if (!gname.empty() && gname[0] == '$') {
                    // Tanımsız DEĞİŞKEN — STRICT (interpreter ile birebir mesaj).
                    // Karar gerekçesi: hedef bandın iki ucu da strict (Go: derleme hatası,
                    // Node: ReferenceError); PHP'den yalnız dağıtım/kitle alınıyor, semantik
                    // değil. İnterpreter (referans motor) zaten strict — sapan taraf VM'di
                    // ve sessiz null üretiyordu (bu turun tüm bug'ları o sınıftandı).
                    // LOOK_WARN_UNDEF=1 → GEÇİŞ MODU: eski lenient davranış + uyarı
                    // (yalnız migration için; interpreter her hâlükârda strict → bu modda
                    // motorlar AYRIŞIR, kalıcı kullanım için değildir).
                    if (g_warn_undef) {
                        R(ins.a) = Value();
                        Logger::instance().log(LogLevel::LOG_WARN, "VM",
                            "Undefined variable read (LOOK_WARN_UNDEF transition mode, returned null): " + gname);
                    } else {
                        throw LookVmError("Undefined variable: " + gname);
                    }
                } else {
                    // "mod::fn" gibi değişken-olmayan isimler: burada ıskalamak NORMAL
                    // (genel CALL yoluna düşer) → fırlatmak yanlış olurdu.
                    R(ins.a) = Value();
                }
                break;
            }
            case OpCode::STORE_GLOBAL: {
                // a=src_reg, b=name_hi, c=name_lo (16-bit constant pool index)
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                // A2: çivilenmiş slot varsa doğrudan yaz (hash yok). Yalnız MEVCUT slot'a
                // yazar (cache ilk find/insert'ten sonra doldurulur → daima geçerli adres).
                if (Value* hit = (*gcache)[ni]) { *hit = R(ins.a); break; }
                const std::string& name = CONST(ni).str_ref();  // kopyasız
                Value& slot = globals_[name];   // yoksa oluştur, varsa bul
                slot = R(ins.a);
                (*gcache)[ni] = &slot;          // sonraki yazma/okuma için çivile
                break;
            }

            // ── Aritmetik — Value'nun operatörlerini kullan ───────────────────
            case OpCode::ADD: {
                // INT+INT fast-path: skip Value::operator+ machinery (arith_check×2,
                // to_int×2, FLOAT-checks) for the common case — measured ~8.5ns/add,
                // ~28× a native add. Any non-INT / overflow falls through to operator+
                // so FLOAT promotion and non-numeric fail-loud semantics are unchanged.
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_add_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b + c;
                break;
            }
            case OpCode::SUB: {
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_sub_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b - c;
                break;
            }
            case OpCode::MUL: {
                const Value& b = R(ins.b); const Value& c = R(ins.c);
                if (b.type() == Value::INT && c.type() == Value::INT) {
                    int64_t r;
                    if (!look::i64_mul_ovf(b.as_int(), c.as_int(), &r)) { R(ins.a) = Value(r); break; }
                }
                R(ins.a) = b * c;
                break;
            }
            case OpCode::DIV:    R(ins.a) = R(ins.b) / R(ins.c);        break;
            case OpCode::MOD:    R(ins.a) = R(ins.b) % R(ins.c);        break;
            case OpCode::POW:    R(ins.a) = R(ins.b).pow(R(ins.c));     break;
            case OpCode::UNM: {
                const Value& b = R(ins.b);
                if (b.type()==Value::FLOAT) {
                    R(ins.a) = Value(-b.as_float());
                } else {
                    // İ ile aynı: INT dışı (STRING/BOOL/NONE) tip to_int ile çevrilir.
                    // Eski `-b.as_float()` ham float_val(0.0)'ı okuyup `-"5"`→-0
                    // veriyordu (İ ile divergence). to_int → -"5"=-5, -true=-1.
                    int64_t v = b.to_int();
                    R(ins.a) = (v == INT64_MIN) ? Value(-(double)v) : Value(-v);
                }
                break;
            }
            case OpCode::BAND:   R(ins.a) = R(ins.b).bitwise_and(R(ins.c)); break;
            case OpCode::BOR:    R(ins.a) = R(ins.b).bitwise_or(R(ins.c));  break;
            case OpCode::BXOR:   R(ins.a) = R(ins.b).bitwise_xor(R(ins.c)); break;
            case OpCode::BNOT:   R(ins.a) = R(ins.b).bitwise_not();         break;
            case OpCode::SHL:    R(ins.a) = R(ins.b).shift_left(R(ins.c));  break;
            case OpCode::SHR:    R(ins.a) = R(ins.b).shift_right(R(ins.c)); break;
            case OpCode::CONCAT:
                // B7: dst==sol operand ise akümülatöre yerinde ekle (amortize O(1)).
                // `.=` ve `$s=$s.x` bu yolu üretir; genel `a.b` (a!=b) eski yolda.
                if (ins.a == ins.b) R(ins.a).append_in_place(R(ins.c));
                else                R(ins.a) = R(ins.b).concat(R(ins.c));
                break;

            // ── Karşılaştırma ─────────────────────────────────────────────────
            case OpCode::EQ:  R(ins.a) = Value(R(ins.b) == R(ins.c)); break;
            case OpCode::NEQ: R(ins.a) = Value(!(R(ins.b) == R(ins.c))); break;
            case OpCode::LT:  R(ins.a) = Value(R(ins.b) <  R(ins.c)); break;
            case OpCode::GT:  R(ins.a) = Value(R(ins.b) >  R(ins.c)); break;
            case OpCode::LTE: R(ins.a) = Value(R(ins.b) <= R(ins.c)); break;
            case OpCode::GTE: R(ins.a) = Value(R(ins.b) >= R(ins.c)); break;
            case OpCode::CMP3: R(ins.a) = Value(R(ins.b).spaceship(R(ins.c))); break;
            case OpCode::NOT: R(ins.a) = Value(!R(ins.b).is_truthy()); break;
            case OpCode::COALESCE:
                R(ins.a) = (R(ins.b).type() != Value::NONE) ? R(ins.b) : R(ins.c);
                break;

            // ── Array ─────────────────────────────────────────────────────────
            case OpCode::NEW_ARRAY: {
                auto v = std::make_shared<std::vector<Value>>();
                v->reserve(ins.b);
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::NEW_ASSOC: {
                // The "__assoc__" sentinel is a compile-time constant; allocating a fresh
                // make_shared<string> for it on every assoc creation was pure waste (5M
                // allocations in the object_create benchmark, and one per JSON response on
                // the web path). Share one immutable instance — push_back is a shared_ptr
                // refcount bump, not an allocation. Thread-safe: magic-static init + atomic
                // refcount, and the sentinel is only ever compared, never mutated.
                static const Value assoc_sentinel{std::string("__assoc__")};
                auto v = std::make_shared<std::vector<Value>>();
                if (ins.b > 0) v->reserve(ins.b);   // compiler-known final size (sentinel + 2/pair)
                v->push_back(assoc_sentinel);
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::ARRAY_PUSH:
                if (R(ins.a).type() == Value::ARRAY)
                    R(ins.a).as_array()->push_back(R(ins.b));
                break;
            case OpCode::ARRAY_GET:
                R(ins.a) = array_get(R(ins.b), R(ins.c));
                break;
            case OpCode::ARRAY_SET:
                array_set(R(ins.a), R(ins.b), R(ins.c));
                break;
            case OpCode::ARRAY_LEN: {
                const Value& arr = R(ins.b);
                int len = 0;
                if (arr.type() == Value::ARRAY) {
                    auto& vec = *arr.as_array();
                    // Sentinel-first: ["__assoc__", k, v, ...] → (size-1)/2 pairs
                    if (!vec.empty() && vec[0].type()==Value::STRING && vec[0].as_string()=="__assoc__") {
                        size_t data_start = 1;
                        if (vec.size()>2 && vec[1].as_string()=="__struct__") data_start=3;
                        len = (int)(vec.size()-data_start)/2;
                    } else {
                        len = (int)vec.size();
                    }
                }
                R(ins.a) = Value(len);
                break;
            }

            // ── Struct — ARRAY + __struct__ sentinel (interpreter uyumlu) ─────
            case OpCode::NEW_STRUCT: {
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                std::string sname = CONST(ni).as_string();
                auto v = std::make_shared<std::vector<Value>>();
                // interpreter convention: ["__assoc__","__struct__","StructName", fields...]
                v->push_back(Value(std::string("__assoc__")));
                v->push_back(Value(std::string("__struct__")));
                v->push_back(Value(sname));
                // Default field değerleri
                if (shared_.struct_defs) {
                    auto it = shared_.struct_defs->find(sname);
                    if (it != shared_.struct_defs->end()) {
                        for (auto& f : it->second) {
                            v->push_back(Value(f.name));
                            v->push_back(f.default_val);
                        }
                    }
                }
                R(ins.a) = Value(v);
                // NOP(field_count) + LOAD_CONST_W*N skip
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    int fc = proto->code[frame.ip].a;
                    frame.ip += 1 + fc;
                }
                break;
            }
            case OpCode::GET_FIELD: {
                uint8_t fi = ins.c;
                const std::string& fname = CONST(fi).as_string();
                R(ins.a) = get_field(R(ins.b), fname);
                break;
            }
            case OpCode::SET_FIELD: {
                uint8_t fi = ins.b;
                const std::string& fname = CONST(fi).as_string();
                set_field(R(ins.a), fname, R(ins.c));
                break;
            }

            // ── Jump ──────────────────────────────────────────────────────────
            case OpCode::JUMP:
                frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_FALSE:
                if (!R(ins.a).is_truthy()) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_TRUE:
                if ( R(ins.a).is_truthy()) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_NULL:
                if (R(ins.a).type()==Value::NONE) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;

            // ── CALL ──────────────────────────────────────────────────────────
            case OpCode::CALL: {
                // a=ret_reg, b=fn_reg, c=args_base; sonraki NOP(argc)
                uint8_t argc = 0;
                uint16_t cname = 0;   // callee adinin sabit indeksi + 1 (0 = ad yok)
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc  = proto->code[frame.ip].a;
                    cname = (uint16_t)((proto->code[frame.ip].b << 8) | proto->code[frame.ip].c);
                    ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN) {
                    // ESKI HATA: mesaj adi soylemiyordu — "Cagrilabilir degil
                    // (BYTECODE_FN bekleniyor)". Hangi ad? tree-walk (REFERANS)
                    // "Undefined variable: <ad>" diyordu; motorlar hem mesajda hem
                    // teshis edilebilirlikte ayrisiyordu (S3).
                    if (cname) {
                        const std::string& nm = CONST((uint16_t)(cname - 1)).str_ref();
                        if (fn_val.type() == Value::NONE)
                            throw LookVmError("Undefined variable: " + nm);
                        throw LookVmError("'" + nm + "' is not callable (function expected)");
                    }
                    throw LookVmError("Not callable (function expected)");
                }
                auto cl = fn_val.as_bytecode_fn();
                auto* cp = cl->proto.get();
                int new_base = (int)regs_.size();
                regs_.resize(new_base + cp->reg_count);
                if (!cp->variadic) {
                    for (int i = 0; i < argc && i < cp->arity; ++i)
                        regs_[new_base + i] = R(ins.c + i);
                } else {
                    // Variadic: sabit paramları bağla, kalanları rest ARRAY'ine topla
                    // (call_closure ile aynı — CALL yolunda eksikti).
                    int vfixed = cp->arity - 1;
                    for (int i = 0; i < vfixed && i < argc; ++i)
                        regs_[new_base + i] = R(ins.c + i);
                    auto varr = std::make_shared<std::vector<look::Value>>();
                    for (int i = vfixed; i < argc; ++i) varr->push_back(R(ins.c + i));
                    if (vfixed >= 0 && cp->arity > 0)
                        regs_[new_base + vfixed] = look::Value(varr);
                }
                // MAX_CALL_DEPTH bytecode CALL yolunda da zorlanmalı (57. bug):
                // eskiden yalnız call_closure (host→VM) kontrol ediyordu; doğrudan
                // LOOK özyinelemesi (CALL opcode) SINIRSIZDI → runaway/derin
                // özyineleme graceful hata yerine bellek tükenene dek gidiyordu
                // (OOM DoS) ve tree-walk'un 256 sınırından ayrışıyordu.
                if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
                    throw LookVmError("Stack overflow (max " + std::to_string(MAX_CALL_DEPTH) + ")");
                int ret_abs = base + ins.a;
                call_stack_.push_back({cl->proto.get(), cl.get(), 0, new_base, ret_abs, (int)argc});
                goto call_dispatch; // callee frame'e geç
            }

            case OpCode::CALL_BUILTIN: {
                // 16-bit builtin indeksi: dusuk 8 bit ins.b'de, YUKSEK 8 bit takip eden
                // NOP hint'inin b alaninda (a=argc). Eski kodlamayla bit-uyumlu: idx<=255
                // icin NOP.b hep 0'di. 8-bit'ken 256. giris SESSIZCE kirpilip yanlis
                // builtin'i cagiriyordu (index 256 → 0 → print) — duvar boyle asildi.
                uint8_t argc2 = 1;
                uint16_t bidx = ins.b;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc2 = proto->code[frame.ip].a;
                    bidx |= (uint16_t)proto->code[frame.ip].b << 8;
                    ++frame.ip;
                }
                if (!shared_.builtins || bidx >= shared_.builtins->size())
                    throw LookVmError("Unknown built-in: " + std::to_string(bidx));
                std::vector<Value> args;
                args.reserve(argc2);
                for (int i = 0; i < argc2; ++i) args.push_back(R(ins.c + i));
                // Modul 'use' edilmemisse tablodaki giris BOS std::function'dir ve
                // cagirmak C++ std::bad_function_call firlatir.
                // ESKI HATA: bu istisna kullaniciya oldugu gibi ciktiyordu:
                //   print(string::len("abc"))   ->  "Runtime Error: bad_function_call"
                // Ne modulu ne fonksiyonu soyluyor, ustelik bir C++ ic terimi. Oysa
                // tree-walk (REFERANS) dogru mesaji veriyordu: "Module 'string' not
                // loaded." — 'use' unutmak LOOK'ta en sik yapilan hata oldugu icin
                // varsayilan motor en kotu mesaji veriyordu. Ayni metin kullaniliyor
                // ki iki motor birebir ayni hatayi versin (S3).
                {
                    const auto& bfn = (*shared_.builtins)[bidx];
                    if (!bfn) {
                        const auto& names = builtin_names();
                        std::string nm = bidx < names.size() ? names[bidx] : std::string();
                        size_t sep = nm.find("::");
                        if (sep != std::string::npos)
                            throw LookVmError("Module '" + nm.substr(0, sep) + "' not loaded.");
                        throw LookVmError("Built-in '" + (nm.empty() ? std::to_string(bidx) : nm) +
                                          "' unavailable (not linked)");
                    }
                    R(ins.a) = bfn(args);
                }
                // Re-entrancy güvenliği: builtin (ör. array::map) callback aracılığıyla
                // call_closure ile VM'e geri girip call_stack_'i realloc etmiş olabilir
                // → dıştaki frame/proto referansları geçersizleşir. call_dispatch'e
                // dönerek yeniden bağla (frame.ip zaten builtin+NOP'un ötesinde).
                goto call_dispatch;
            }

            case OpCode::RETURN: {
                Value ret = R(ins.a);
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = std::move(ret);
                    goto call_dispatch; // caller frame'e dön
                } else {
                    return ret;
                }
            }
            case OpCode::RETURN_NULL: {
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = Value();
                    goto call_dispatch; // caller frame'e dön
                } else {
                    return Value();
                }
            }
            case OpCode::TAIL_CALL: {
                // Aynı CALL mantığı — frame yeniden kullan (tail call opt)
                // Şimdilik normal CALL gibi davran — optimize edilmemiş
                uint8_t argc3 = 0;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc3 = proto->code[frame.ip].a; ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN)
                    throw LookVmError("TAIL_CALL: BYTECODE_FN expected");
                auto cl = fn_val.as_bytecode_fn();
                auto* cp = cl->proto.get();
                int new_base = (int)regs_.size();
                regs_.resize(new_base + cp->reg_count);
                if (!cp->variadic) {
                    for (int i = 0; i < argc3 && i < cp->arity; ++i) regs_[new_base+i] = R(ins.c+i);
                } else {
                    int vfixed = cp->arity - 1;
                    for (int i = 0; i < vfixed && i < argc3; ++i) regs_[new_base+i] = R(ins.c+i);
                    auto varr = std::make_shared<std::vector<look::Value>>();
                    for (int i = vfixed; i < argc3; ++i) varr->push_back(R(ins.c+i));
                    if (vfixed >= 0 && cp->arity > 0) regs_[new_base+vfixed] = look::Value(varr);
                }
                // MAX_CALL_DEPTH — CALL_METHOD/ikinci CALL yolu (57. bug, bkz. yukarı)
                if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
                    throw LookVmError("Stack overflow (max " + std::to_string(MAX_CALL_DEPTH) + ")");
                call_stack_.push_back({cp, cl.get(), 0, new_base, base+ins.a, (int)argc3});
                goto call_dispatch; // callee frame'e geç
            }

            // ── Closure ───────────────────────────────────────────────────────
            case OpCode::MAKE_CLOSURE: {
                auto& nested_proto = proto->nested[ins.b];
                auto cl = std::make_shared<Closure>(nested_proto);
                // Capture hint'leri oku: LOAD_CAPTURE(0, cr) pattern
                while (frame.ip < (int)proto->code.size()) {
                    const auto& h = proto->code[frame.ip];
                    if (h.op != OpCode::LOAD_CAPTURE || h.a != 0) break;
                    cl->captures.push_back(R(h.b));
                    ++frame.ip;
                }
                R(ins.a) = Value(cl);
                break;
            }
            case OpCode::LOAD_CAPTURE: {
                if (!frame.closure || ins.b >= frame.closure->captures.size())
                    throw LookVmError("Capture index out of range: " + std::to_string(ins.b));
                R(ins.a) = frame.closure->captures[ins.b];
                break;
            }

            // ── Foreach ───────────────────────────────────────────────────────
            case OpCode::FOR_PREP:
                // r_iter = array copy, r_iter+1 = index 0
                R(ins.a)   = R(ins.b);
                R(ins.a+1) = Value(0);
                break;
            case OpCode::FOR_STEP: {
                const Value& arr = R(ins.a);
                int idx = R(ins.a+1).as_int();
                if (arr.type() != Value::ARRAY) { frame.ip=(uint16_t(ins.b)<<8)|ins.c; break; }
                auto& vec = *arr.as_array();
                // Sentinel-first: ["__assoc__", k0, v0, k1, v1, ...]
                bool is_assoc = !vec.empty() && vec[0].type()==Value::STRING
                                && vec[0].as_string()=="__assoc__";
                if (is_assoc) {
                    // Skip __assoc__ sentinel + optional __struct__ tag
                    int data_start = 1;
                    if (vec.size()>2 && vec[1].type()==Value::STRING && vec[1].as_string()=="__struct__")
                        data_start = 3;
                    int pi = data_start + idx * 2;
                    if (pi + 1 >= (int)vec.size()) {
                        frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                    } else {
                        R(ins.a+3) = vec[pi];     // key
                        R(ins.a+2) = vec[pi+1];   // val
                        R(ins.a+1) = Value(idx+1);
                    }
                } else {
                    if (idx >= (int)vec.size()) {
                        frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                    } else {
                        R(ins.a+2) = vec[idx];    // val
                        R(ins.a+3) = Value(idx);  // key
                        R(ins.a+1) = Value(idx+1);
                    }
                }
                break;
            }

            // ── Try/Catch ─────────────────────────────────────────────────────
            case OpCode::TRY_PUSH: {
                int catch_ip = (uint16_t(ins.b)<<8)|ins.c;
                try_stack_.push_back({catch_ip, (int)call_stack_.size(), base});
                break;
            }
            case OpCode::TRY_POP:
                if (!try_stack_.empty()) try_stack_.pop_back();
                break;
            case OpCode::THROW: {
                current_exception_ = R(ins.a);
                // Bu run()'a ait handler yoksa: değeri C++ sınırından propagate et.
                // (Dış run()'ın handler'ına atlamak call stack'i bozar ve aradaki
                //  C++ katmanının catch'ini — ör. db::transaction ROLLBACK — atlar.)
                if (try_stack_.size() <= try_floor_)
                    throw LookVmThrow(current_exception_);
                auto entry = try_stack_.back(); try_stack_.pop_back();
                while ((int)call_stack_.size() > entry.frame_depth) {
                    regs_.resize(call_stack_.back().base);
                    call_stack_.pop_back();
                }
                call_stack_.back().ip = entry.catch_ip;
                // frame/proto/base'i catch'in bulunduğu frame'e YENİDEN BAĞLA (RETURN ile
                // aynı). Salt `break` inner loop'u bayat `proto` + sarkan `frame` referansıyla
                // sürdürüyordu (fonksiyon-sınırını geçen throw → sessiz duruş/UB; VM≠interpreter).
                goto call_dispatch;
            }
            case OpCode::LOAD_EXC:
                R(ins.a) = current_exception_;
                break;
            case OpCode::LOAD_ARGC:
                R(ins.a) = Value((int64_t)frame.argc);
                break;

            // ── LOOK'a özgü ───────────────────────────────────────────────────
            case OpCode::PARALLEL_CALL: {
                if (R(ins.a).type() != Value::BYTECODE_FN)
                    throw LookVmError("parallel(): BYTECODE_FN expected");
                task_acquire(); // THROW mode: throws if LOOK_PARALLEL_LIMIT reached
                auto src_cl = R(ins.a).as_bytecode_fn();
                // 58: CELL captures'ı deep-clone et. Eskiden captures salt-okunur
                // snapshot'tı (thread-safe). By-ref cell'e geçince, aynı closure iki
                // thread'de paylaşılan cell'i (aynı shared_ptr array) görür; biri
                // cell[0]'a yazarsa sessiz veri yarışı. proto.capture_is_cell hangi
                // capture'ın cell olduğunu söyler; yalnız onları deep-clone et.
                std::shared_ptr<Closure> cl_copy = src_cl;
                {
                    const auto& isc = src_cl->proto->capture_is_cell;
                    // Klonlanacak capture: CELL (mutable → paylaşım=yarış) VEYA CLOSURE
                    // (BYTECODE_FN). Closure'ı klonlamak şart çünkü içindeki cell'ler aksi
                    // halde parent'la paylaşımlı kalır (np1: parallel task bir closure
                    // yakalar, closure-içi cell'i parent değiştirir → veri yarışı). deep_clone
                    // BYTECODE_FN'i transitif klonlar (bc_fn_cloner hook, aşağıda kayıtlı).
#ifdef LOOK_NO_TRANSITIVE_CLONE
                    constexpr bool CLONE_CLOSURES = false; // TSan pozitif kontrol: eski (yarışlı) hâl
#else
                    constexpr bool CLONE_CLOSURES = true;
#endif
                    // R-02: capture izolasyonu — İZİN LİSTESİ (yalnız cell+closure klonla)
                    // yerine YASAK LİSTESİ (fail-safe). Paylaşımı KASITLI handle'lar
                    // (CHANNEL/WS/SSE — cross-thread iletişim) DIŞINDA her capture klonlanır.
                    // Eski hâl düz ARRAY capture'ı sığ paylaşıyordu → parallel task'lar aynı
                    // vector'ü mutasyona uğratıp YARIŞIYORDU (t1b VM, TSan). deep_clone_impl
                    // her tipi doğru ele alır (array özyineli, handle *this). Yeni Value tipi
                    // eklenince otomatik güvenli tarafta başlar. (CLONE_CLOSURES=false =
                    // LOOK_NO_TRANSITIVE_CLONE pozitif-kontrol: eski yarışlı hâl.)
                    auto must_clone = [](Value::Type t) {
                        return t != Value::CHANNEL && t != Value::WEBSOCKET && t != Value::SSE_CONN;
                    };
                    bool needs = false;
                    for (size_t i = 0; i < src_cl->captures.size(); ++i) {
                        bool cell = (i < isc.size() && isc[i]);
                        if (cell || (CLONE_CLOSURES && must_clone(src_cl->captures[i].type()))) { needs = true; break; }
                    }
                    if (needs) {
                        cl_copy = std::make_shared<Closure>(src_cl->proto);
                        cl_copy->captures = src_cl->captures;   // sığ (shared_ptr paylaşır)
                        for (size_t i = 0; i < cl_copy->captures.size(); ++i) {
                            bool cell = (i < isc.size() && isc[i]);
                            if (cell || (CLONE_CLOSURES && must_clone(cl_copy->captures[i].type())))
                                cl_copy->captures[i] = cl_copy->captures[i].clone_for_thread();
                        }
                    }
                }
                SharedState sh = shared_;
                std::unordered_map<std::string, Value> g_copy = globals_;
                // builtins ve routes local pointer'lara işaret eder — parallel task için deep copy
                std::vector<BuiltinFn> builtins_copy = sh.builtins ? *sh.builtins : std::vector<BuiltinFn>{};
                sh.builtins = nullptr;
                sh.routes   = nullptr; // parallel task dispatch_routes çağırmaz
                std::thread([cl_copy, sh, g_copy, builtins_copy = std::move(builtins_copy)]() mutable {
                    TaskGuard _guard; // task_release() on scope exit
                    // DB bağlantısı iadesi — interpreter parallel() ile birebir: task
                    // içinde db::query çağrılırsa get_conn bu thread'e tembel bir conn
                    // alır; bırakılmazsa havuzdan KALICI sızar (birkaç istek sonra
                    // havuz tükenir, endpoint sonsuza dek asılır).
                    struct DbGuard { ~DbGuard() { look::release_thread_connections(); } } _db;
                    std::ostringstream sink;
                    VM tvm(sh, sink);
                    tvm.set_globals(std::move(g_copy));
                    tvm.set_builtins(&builtins_copy);
                    try { tvm.call_closure(*cl_copy, {}); }
                    catch (const std::exception& e) {
                        Logger::instance().log(LogLevel::LOG_ERROR, "VM",
                            std::string("parallel() panic: ") + e.what());
                    } catch (...) {
                        Logger::instance().log(LogLevel::LOG_ERROR, "VM",
                            "parallel() panic: unknown error");
                    }
                }).detach();
                break;
            }
            // CHAN_* — TİP KONTROLÜ ŞART: as_channel() type_ bakmadan static_pointer_cast
            // yapar; argüman ARRAY/STRING/FN ise (compiler send/receive/close/chan_size'ı
            // tipe bakmadan üretir) o nesnenin belleği LookChannel sanılır → TYPE CONFUSION
            // → heap-buffer-overflow (ASan-kanıtlı). interpreter.cpp:1646 zaten kontrol
            // ediyordu; VM'de yoktu (motor ayrışması, S3). send([1,2,3],42) gibi.
            case OpCode::CHAN_SEND: {
                if (R(ins.a).type() != Value::CHANNEL)
                    throw LookVmError("send(): argument is not a channel");
                R(ins.a).as_channel()->send_val(R(ins.b));
                break;
            }
            case OpCode::CHAN_RECV: {
                if (R(ins.b).type() != Value::CHANNEL)
                    throw LookVmError("receive(): argument is not a channel");
                R(ins.a) = R(ins.b).as_channel()->recv_val();
                break;
            }
            case OpCode::CHAN_CLOSE: {
                if (R(ins.a).type() != Value::CHANNEL)
                    throw LookVmError("close(): argument is not a channel");
                auto ch = R(ins.a).as_channel();
                { std::unique_lock<std::mutex> lk(ch->mtx); ch->closed = true; ch->not_empty.notify_all(); }
                break;
            }
            case OpCode::CHAN_SIZE: {
                if (R(ins.b).type() != Value::CHANNEL)
                    throw LookVmError("chan_size(): argument is not a channel");
                R(ins.a) = Value(R(ins.b).as_channel()->sz());
                break;
            }

            // ── Cast / Type ───────────────────────────────────────────────────
            case OpCode::TO_INT:   R(ins.a) = Value(R(ins.b).to_int());   break;
            case OpCode::TO_FLOAT: R(ins.a) = Value(R(ins.b).to_float()); break;
            case OpCode::TO_STR:   R(ins.a) = Value(R(ins.b).to_string()); break;
            case OpCode::TO_BOOL:  R(ins.a) = Value(R(ins.b).is_truthy()); break;
            case OpCode::TYPE_OF: {
                static const char* tnames[] = {
                    "int","float","string","bool","function","array",
                    "channel","websocket","sse","function","null"
                };
                int ti = (int)R(ins.b).type();
                R(ins.a) = Value(std::string(ti < 11 ? tnames[ti] : "unknown"));
                break;
            }

            case OpCode::NOP:       break;
            case OpCode::BREAKPOINT:
                Logger::instance().log(LogLevel::LOG_DEBUG, "VM", "BREAKPOINT");
                break;

            default:
                throw LookVmError("Unknown opcode: " + std::to_string((int)ins.op));

#undef R
#undef CONST
            } // switch
        } // inner while
        }
        // ── C++ exception → LOOK try/catch köprüsü ────────────────────────────
        // Operatör/builtin'lerin fırlattığı runtime hatası (bölme sıfıra, tip
        // hatası...) try_stack_'e yönlendirilir — interpreter semantiğiyle birebir:
        // kontrol-akışı exception'ları (exit/route/stop/route-disabled) sızdırılır,
        // geri kalan std::exception mesajıyla catch bloğuna düşer. try_stack_ boşsa
        // yukarı iletilir (web'de route fallback, CLI-VM'de üst seviye hata).
        catch (const VmRouteDisabled&)      { throw; }
        catch (const RouteStopException&)   { throw; }
        catch (const RouteMatchedException&){ throw; }
        catch (const ExitException&)        { throw; }
        catch (const LookVmThrow& t) {
            // C++ sınırını geçmiş LOOK throw (iç run() → builtin/interpreter → buraya).
            // DEĞER korunur (error::new gibi tipli değerler dahil) — e.what() değil.
            if (try_stack_.size() <= try_floor_) { note_error_line(frame, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = t.value;
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }
        catch (const LookRuntimeError& e) {
            // error::new() DEĞER DÖNDÜRMEZ — doğrudan LookRuntimeError(payload) fırlatır.
            // Tipli payload'ı korumak ZORUNLU: e.what() string'ine düşürmek catch
            // değişkenini "[type, E_DB, message, ...]" string'i yapar → error::is()
            // false döner, error::message() tüm nesneyi verir (sessizce yanlış hata
            // yönetimi). interpreter.cpp'deki catch ile birebir: has_value ? value : message.
            if (try_stack_.size() <= try_floor_) { note_error_line(frame, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = e.has_value ? e.value : Value(e.message);
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }
        catch (const std::exception& e) {
            if (try_stack_.size() <= try_floor_) { note_error_line(frame, proto); throw; }
            auto entry = try_stack_.back(); try_stack_.pop_back();
            current_exception_ = Value(std::string(e.what()));
            while ((int)call_stack_.size() > entry.frame_depth) {
                regs_.resize(call_stack_.back().base);
                call_stack_.pop_back();
            }
            call_stack_.back().ip = entry.catch_ip;
            goto call_dispatch;
        }

        // Implicit return null
        int ret_reg = frame.ret_reg;
        regs_.resize(base);
        call_stack_.pop_back();
        if (call_stack_.empty() || ret_reg < 0) return Value();
        regs_[ret_reg] = Value();

    } // outer while
    return Value();
}

} // namespace look

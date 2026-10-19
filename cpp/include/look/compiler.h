#pragma once

#include "look/ast.h"
#include "look/bytecode.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <stack>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <set>

namespace look {

// ── Compile-time hata ─────────────────────────────────────────────────────────

class LookCompileError : public std::runtime_error {
public:
    int line;
    explicit LookCompileError(const std::string& msg, int line = 0)
        : std::runtime_error(msg), line(line) {}
};

// ── Kontrollü daraltma ───────────────────────────────────────────────────────
// Bytecode operandları 16 bit (LOOK 2: eskiden 8 bitti — 255 register/parametre/yakalama,
// 65535 sabit sınırı; "VM derleyemezse uygulama başlamaz" kuralından önce kaldırıldı).
// İki operand birleşince 32 bitlik alan taşır (b<<16|c). Derleyicide bir değeri operanda
// sığdıran HER daraltma buradan geçer: sığmıyorsa derleme hatası. Çıplak daraltma dört ayrı
// sessiz hataya yol açmıştı (değer sınırda başa sarıyor, VM hatasız YANLIŞ çalışıyordu).
// compiler.cpp'de çıplak daraltma YASAK — tests/no_unchecked_narrowing.sh CI'da denetler.
using Reg = uint16_t;
constexpr Reg NO_REG = 0xFFFF;   // compile_expr(dest): "geçici register seç" (geçerli register değil)
constexpr long long REG_LIMIT = 0xFFFE;   // en yüksek register indeksi (NO_REG ayrılmış)
template <class T> inline uint16_t u16(T v, const char* what) {
    if ((long long)v < 0 || (long long)v > REG_LIMIT)
        throw LookCompileError(std::string(what) + " exceeds the VM limit of 65534");
    return static_cast<uint16_t>(v);
}
template <class T> inline uint32_t u32(T v, const char* what) {
    if ((long long)v < 0 || (long long)v > 0xFFFFFFFFll)
        throw LookCompileError(std::string(what) + " exceeds the VM limit of 4294967295");
    return static_cast<uint32_t>(v);
}
// 32-bit alanın yüksek/düşük 16 biti (b = hi, c = lo — komuta göre sıra değişir).
template <class T> inline uint16_t hi16(T v) { return static_cast<uint16_t>(u32(v, "32-bit operand") >> 16); }
template <class T> inline uint16_t lo16(T v) { return static_cast<uint16_t>(u32(v, "32-bit operand") & 0xFFFF); }
// Yalnız kapasite ipucu (reserve): doygunlaşır, anlam taşımaz.
inline uint16_t hint_u16(size_t n) { return static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n); }
// LOAD_INT'in işaretli 16-bit anlık değeri.
inline uint16_t i16_bits(long long v) {
    if (v < -32768 || v > 32767) throw LookCompileError("immediate integer out of range");
    return static_cast<uint16_t>(static_cast<int16_t>(v));
}

// ── RegisterAllocator ─────────────────────────────────────────────────────────
//
// Local değişkenler 0..num_locals-1 arasında sabit slot alır.
// Temp değerler num_locals'tan başlar, free() ile geri verilir.
// Max 65534 register (16-bit operand; 0xFFFF = NO_REG) — aşılırsa LookCompileError.

class RegisterAllocator {
public:
    explicit RegisterAllocator(Reg locals_end, std::string owner = "<main>")
        : locals_end_(locals_end), next_(locals_end), max_(locals_end),
          owner_(std::move(owner)) { grow(locals_end); }

    // Register operandı 16 bit → fonksiyon başına en fazla 65534 register (LOOK 2; v1'de
    // 8 bit / 255'ti ve aşılınca TÜM program yorumlayıcıya düşüyordu).
    [[noreturn]] void too_many() const {
        throw LookCompileError("function '" + owner_ + "' is too large for the VM: it needs "
                               "more than 65534 registers (local variables + temporaries). "
                               "Split it into smaller functions.");
    }

    Reg alloc() {
        Reg r;
        if (!free_.empty()) {
            r = free_.back();
            free_.pop_back();
            in_free_[r] = false;
        } else {
            if (next_ >= REG_LIMIT) too_many();
            r = next_++;
            grow(next_);
        }
        if (r + 1 > max_) max_ = r + 1;
        return r;
    }

    void free(Reg r) {
        // Pinned (aktif local) register'lar havuza dönmez: compile_expr bir
        // local'in slot'unu doğrudan döndürüp çağıran free_temp edince, o slot
        // yanlışlıkla yeniden kullanılıp local'i bozardı (fonksiyon-local'ler
        // temp aralığında olduğundan locals_end_ koruması yetmiyor).
        // Canlı bir ardışık bloğun (seq_) register'ı da dönmez: argüman derlenirken
        // compile_expr base+k'yı döndürür, çağıran free_temp eder — blok hâlâ kullanımda.
        // (SAVUNMA AMAÇLI: seq_ koruması kapatılıp tüm guardlar + iç içe çağrı/döngü/üçlü
        // probları koşuldu, hiçbiri kırılmadı — gerekli olduğu KANITLANMADI, ayırıcı en riskli
        // yer olduğu için tutuluyor.)
        // in_free_: çift free aynı register'ı iki kez dağıtmasın.
        if (r >= locals_end_ && !pinned_[r] && !seq_[r] && !in_free_[r]) {
            free_.push_back(r);
            in_free_[r] = true;
        }
    }

    // Korumalı local slot ayır — free() bunu havuza atmaz (pop_scope'ta çözülür).
    Reg alloc_local() {
        Reg r = alloc();
        pinned_[r] = true;
        return r;
    }
    // Local scope'tan çıkınca: pin'i kaldır + register'ı havuza iade et.
    void free_local(Reg r) {
        pinned_[r] = false;
        free(r);
    }

    // n ardışık register ayır (VM çağrı argümanlarını base+k'da bekler). Blok canlıyken
    // seq_ ile korunur; iş bitince release_seq() ile havuza döner.
    //
    // ESKİ HATA (register sızıntısı): blok HER ZAMAN next_'ten alınıyor ve korumak için
    // locals_end_ yukarı çekiliyordu. locals_end_ bir daha inmediği için hem blok hem de
    // ondan önce ayrılmış tüm geçici register'lar KALICI olarak kayboluyordu: argümanlı
    // her çağrı ~argc+1 register tüketiyordu → bir fonksiyonda en fazla 72 route() /
    // 63 üç-argümanlı çağrı; aşılınca TÜM program yorumlayıcıya düşüyordu.
    // Şimdi: önce serbest havuzda n'lik ardışık bir boşluk aranır, yoksa next_'ten alınır.
    Reg alloc_seq(Reg n) {
        if (n == 0) return next_;
        int base = -1;
        for (int b = locals_end_; b + n <= next_; ++b) {
            int k = 0;
            while (k < n && in_free_[b + k]) ++k;
            if (k == n) { base = b; break; }
            b += k;   // b+k serbest değil → ondan sonrasından devam
        }
        if (base >= 0) {
            for (int k = 0; k < n; ++k) in_free_[base + k] = false;
            free_.erase(std::remove_if(free_.begin(), free_.end(),
                            [&](Reg r) { return r >= base && r < base + n; }),
                        free_.end());
        } else {
            if ((long long)next_ + n > REG_LIMIT) too_many();
            base = next_;
            next_ = static_cast<Reg>(next_ + n);
            grow(next_);
            if (next_ > max_) max_ = next_;
        }
        for (int k = 0; k < n; ++k) seq_[base + k] = true;
        return static_cast<Reg>(base);
    }

    // alloc_seq bloğunu bırak: koruma kalkar, register'lar havuza döner.
    void release_seq(Reg base, Reg n) {
        for (int k = 0; k < n; ++k) { seq_[base + k] = false; free(static_cast<Reg>(base + k)); }
    }

    Reg max_used() const { return max_; }

private:
    // Bayrak dizileri register sayısıyla büyür (eskiden sabit 256'lık diziler).
    void grow(size_t n) {
        if (n > in_free_.size()) { in_free_.resize(n, 0); seq_.resize(n, 0); pinned_.resize(n, 0); }
    }
    Reg locals_end_;
    Reg next_;
    Reg max_;
    std::vector<Reg>  free_;         // LIFO (alloc sondan alır)
    std::vector<char> in_free_;      // free_ üyeliği (çift free + ardışık blok araması)
    std::vector<char> seq_;          // canlı alloc_seq bloğu (free() atlar)
    std::vector<char> pinned_;       // aktif local register'lar (free() atlar)
    std::string owner_;              // hata mesajı için fonksiyon adı
};

// ── LocalVar — lexical scope içindeki değişken ───────────────────────────────

struct LocalVar {
    std::string name;
    uint16_t     reg;
    int         depth;
};

// ── Capture — closure use() listesi ─────────────────────────────────────────

struct CaptureInfo {
    std::string name;
    uint16_t     capture_index; // Closure.captures[] sırası
};

// ── Loop stack — break/continue patch ────────────────────────────────────────

struct LoopContext {
    std::vector<int> break_patches;
    std::vector<int> continue_patches;
    int              continue_target = -1; // loop başı IP
    bool             is_switch = false;    // switch: break'i yakalar, continue'yu
                                           // dıştaki döngüye geçirir (C semantiği)
    size_t           finally_floor = 0;    // loop girişindeki pending_finally_ derinliği;
                                           // break/continue bu dereye kadarki finally'leri çalıştırır
                                           // (döngü-içi try-finally), dıştakileri DEĞİL (return farkı).
};

// ── FunctionCompiler — tek fonksiyon/closure için ───────────────────────────

class FunctionCompiler {
public:
    FunctionCompiler(const std::string& name,
                     const std::vector<std::string>& params,
                     bool variadic,
                     FunctionCompiler* parent = nullptr);

    std::shared_ptr<FunctionProto> compile(const BlockStatement& body,
        const std::vector<std::unique_ptr<Expression>>* defaults = nullptr);
    std::shared_ptr<FunctionProto> compile_stmts(const std::vector<std::unique_ptr<Statement>>& stmts);

    // Programda builtin OLMAYAN "mod::fn" cagrisi goruldu mu? Compiler::compile bunu
    // CompiledProgram'a tasir → CLI-VM tree-walk'a duser (bkz. bytecode.h aciklamasi).
    bool used_non_builtin_module_fn() const { return non_builtin_module_fn_; }
    const std::vector<std::string>& non_builtin_module_fn_names() const { return non_builtin_names_; }

private:
    // ── Emit ──────────────────────────────────────────────────────────────────
    int  emit(OpCode op, uint16_t a=0, uint16_t b=0, uint16_t c=0);
    int  emit_jump(OpCode op, uint16_t cond_reg=0);  // hedef sonradan patch edilir
    void patch_jump(int offset, int target);
    int  current_ip() const { return (int)proto_.code.size(); }
    // Derlenen statement satiri — emit() proto_.lines'a bunu yazar (VM hata konumu).
    int  cur_line_ = 0;

    // ── Constant pool ──────────────────────────────────────────────────────────
    uint32_t add_const(Value v);
    // Argüman sayısı 16 bit taşınır (v1'de 8 bitti: 255 üstü sessizce sarıyordu, 300 → 44).
    static void check_argc(const CallExpression& e) {
        if ((long long)e.arguments.size() > REG_LIMIT)
            throw LookCompileError("a call has more than 65534 arguments", e.loc.line);
    }
    // MAKE_CLOSURE: a=r, b/c = 32-bit nested-proto indeksi (b düşük, c yüksek).
    void emit_make_closure(uint16_t r, int fn_idx) {
        emit(OpCode::MAKE_CLOSURE, r, lo16(fn_idx), hi16(fn_idx));
    }
    void     emit_load_const(uint16_t dest, Value v, int line);

    // ── Register ──────────────────────────────────────────────────────────────
    uint16_t alloc_temp();
    void    free_temp(uint16_t r);

    // ── Scope ──────────────────────────────────────────────────────────────────
    void    push_scope();
    void    pop_scope();
    uint16_t declare_local(const std::string& name, int line);

    enum class VarKind { LOCAL, CAPTURE, GLOBAL };
    struct VarLoc { VarKind kind; uint16_t index; };
    VarLoc  resolve_var(const std::string& name, bool for_write = false);

    // ── Yerel / yakalanan erişim yardımcıları ─────────────────────────────────
    // Tüm yerel ve yakalanan değişken erişimi bu üç noktadan geçer.
    void emit_read_local(uint16_t dest, uint16_t slot);   // dest = local(slot)
    void emit_write_local(uint16_t slot, uint16_t src);   // local(slot) = src
    void emit_read_capture(uint16_t dest, uint16_t cap_index); // dest = capture

    // ── Expression → register ─────────────────────────────────────────────────
    // dest=NO_REG → compiler geçici register seçer; caller free_temp() çağırmalı
    uint16_t compile_expr(const Expression& expr, uint16_t dest = NO_REG);

    uint16_t compile_binary(const BinaryExpression& e, uint16_t dest);
    uint16_t compile_logical(const BinaryExpression& e, uint16_t dest); // && ||
    uint16_t compile_call(const CallExpression& e, uint16_t dest);
    uint16_t compile_closure(const FunctionExpression& e, uint16_t dest);
    uint16_t compile_string_interp(const std::string& raw, int line, uint16_t dest);
    uint16_t compile_array_lit(const ArrayLiteral& e, uint16_t dest);
    uint16_t compile_assoc_lit(const AssocArrayLiteral& e, uint16_t dest);
    uint16_t compile_struct_lit(const StructLiteralExpression& e, uint16_t dest);

    // ── Statement ─────────────────────────────────────────────────────────────
    void compile_stmt(const Statement& stmt);
    void compile_block(const BlockStatement& block);

    void compile_if(const IfStatement& s);
    void compile_while(const WhileStatement& s);
    void compile_for(const ForStatement& s);
    void compile_foreach(const ForeachStatement& s);
    void compile_return(const ReturnStatement& s);
    void compile_try(const TryCatchStatement& s);
    // BUG FIX (VM finally-on-return): try/catch içinde AKTİF finally blokları. `return`
    // RETURN'den ÖNCE bunları (içten-dışa) emit eder — yoksa RETURN inline finally'yi atlar
    // (interpreter'da finally return'de çalışır; VM'de çalışmıyordu → differential ayrışma).
    std::vector<const BlockStatement*> pending_finally_;
    // floor'a kadarki (dahil değil) bekleyen finally'leri içten-dışa emit et. return→floor=0
    // (fonksiyona kadar hepsi); break/continue→loop'un finally_floor'u (yalnız döngü-içi).
    void emit_pending_finallys(size_t floor = 0);
    // print/write ortak yolu: argümanları " " ile ayırıp yazar, newline=true ise "\n" ekler
    // (interpreter build_output + PrintStatement semantiğiyle birebir).
    void emit_output_args(const std::vector<std::unique_ptr<Expression>>& exprs, bool newline);
    // CALL_BUILTIN indeks alani (b + sonraki NOP.b) 32 bit; eskiden 8 bitti ve 255 ustu SESSIZCE kirpiliyordu.
    // Sigmazsa burasi gurultulu hata verir.
    static void check_builtin_index(int bidx, const std::string& name);

    // Builtin OLMAYAN "mod::fn" cagrisi gorulunce KOK compiler'da isaretle (closure'lar
    // alt-compiler'da derlenir → parent_ zinciriyle koke cikilir). Compiler::compile bunu
    // CompiledProgram'a tasir; CLI-VM bayragi gorunce tree-walk'a duser (runtime'da
    // "Cagirilabilir degil" ile cokmek yerine).
    void mark_non_builtin_module_fn(const std::string& name) {
        FunctionCompiler* c = this;
        while (c->parent_) c = c->parent_;
        c->non_builtin_module_fn_ = true;
        // Ad da tutulur: LOOK_VM_STRICT'te CLI "hangi fonksiyon yüzünden VM'i
        // kullanamıyorum" diye söyleyebilsin (kırmızı liste = VM'e eklenecekler).
        for (auto& n : c->non_builtin_names_) if (n == name) return;
        c->non_builtin_names_.push_back(name);
    }
    bool non_builtin_module_fn_ = false;
    std::vector<std::string> non_builtin_names_;
    void compile_func_decl(const FunctionDeclaration& s);
    void compile_switch(const SwitchStatement& s);
    void compile_const_block(const ConstBlock& s);
    void compile_struct_decl(const StructDeclaration& s);
    void compile_assign_expr(const AssignmentExpression& e);
    // Eleman/alan ataması İFADE olarak kullanıldığında değerinin konacağı yazmaç (-1: istenmiyor).
    int assign_result_reg_ = -1;

    // ── Data ──────────────────────────────────────────────────────────────────
    FunctionProto                    proto_;
    std::unordered_map<std::string, uint32_t> const_index_;   // add_const tekilleştirme (sayı/metin)
    std::unique_ptr<RegisterAllocator> regs_;

    std::vector<LocalVar>            locals_;
    int                              scope_depth_ = 0;
    bool                             callee_ctx_  = false; // çıplak ad ÇAĞRI HEDEFİ olarak derleniyor
    std::set<std::string>            outer_globals_;   // üst düzeyde (blok dışı) atanmış adlar:
                                                       // bir blok bu ada atarsa yeni yerel
                                                       // yaratmaz, global'e yazar
    std::vector<CaptureInfo>         captures_;  // use() listesi + otomatik yakalananlar

    std::vector<LoopContext>         loop_stack_;  // back() = en iç bağlam

    // iota state — const block içinde
    int                              iota_val_  = 0;
    const Expression*                iota_expr_ = nullptr; // tekrarlanan ifade

    FunctionCompiler*                parent_ = nullptr; // capture için
};

// ── Compiler — public API ─────────────────────────────────────────────────────

class Compiler {
public:
    // base_dir: directory of the main script — the root for resolving `use "file.lk"`
    // (file includes) and the sandbox boundary. Empty → file includes are not compiled
    // (they NOP → interpreter fallback, the pre-existing behaviour).
    static CompiledProgram compile(const Program& program, const std::string& base_dir = "");
};

} // namespace look

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
// Bytecode operandları 8/16 bit. Derleyicide bir değeri operanda sığdıran HER daraltma
// buradan geçer: sığmıyorsa derleme hatası. Çıplak (uint8_t)x / & 0xFF dört ayrı sessiz
// hataya yol açtı (alan adı, closure, argüman sayısı, parametre indeksi: değer 256'da
// başa sarıyor, VM hatasız YANLIŞ çalışıyordu). compiler.cpp'de çıplak daraltma YASAK —
// tests/no_unchecked_narrowing.sh CI'da denetler.
template <class T> inline uint8_t u8(T v, const char* what) {
    if ((long long)v < 0 || (long long)v > 0xFF)
        throw LookCompileError(std::string(what) + " exceeds the VM limit of 255");
    return static_cast<uint8_t>(v);
}
template <class T> inline uint16_t u16(T v, const char* what) {
    if ((long long)v < 0 || (long long)v > 0xFFFF)
        throw LookCompileError(std::string(what) + " exceeds the VM limit of 65535");
    return static_cast<uint16_t>(v);
}
// 16-bit operandın yüksek/düşük baytı (b = hi, c = lo). Değer 16 bite sığmalı.
template <class T> inline uint8_t hi8(T v) { return static_cast<uint8_t>(u16(v, "16-bit operand") >> 8); }
template <class T> inline uint8_t lo8(T v) { return static_cast<uint8_t>(u16(v, "16-bit operand") & 0xFF); }
// Yalnız kapasite ipucu (reserve): doygunlaşır, anlam taşımaz.
inline uint8_t hint_u8(size_t n) { return static_cast<uint8_t>(n > 255 ? 255 : n); }
// LOAD_INT'in işaretli 8-bit anlık değeri.
inline uint8_t i8_bits(long long v) {
    if (v < -128 || v > 127) throw LookCompileError("immediate integer out of range");
    return static_cast<uint8_t>(static_cast<int8_t>(v));
}

// ── RegisterAllocator ─────────────────────────────────────────────────────────
//
// Local değişkenler 0..num_locals-1 arasında sabit slot alır.
// Temp değerler num_locals'tan başlar, free() ile geri verilir.
// Max 255 register — aşılırsa LookCompileError.

class RegisterAllocator {
public:
    explicit RegisterAllocator(uint8_t locals_end, std::string owner = "<main>")
        : locals_end_(locals_end), next_(locals_end), max_(locals_end),
          owner_(std::move(owner)) {}

    // v1 bytecode register operandı 8 bit → fonksiyon başına en fazla 255 register.
    // Aşılınca derleme başarısız olur ve (CLI ve web'de) TÜM program yorumlayıcıya
    // düşer. Eskiden Türkçe ve fonksiyon adı olmadan atılıyordu → hangi fonksiyonun
    // bölünmesi gerektiği görünmüyordu. Kalıcı çözüm (sınırsız sanal register) v2'de.
    [[noreturn]] void too_many() const {
        throw LookCompileError("function '" + owner_ + "' is too large for the VM: it needs "
                               "more than 255 registers (local variables + temporaries). "
                               "Split it into smaller functions.");
    }

    uint8_t alloc() {
        uint8_t r;
        if (!free_.empty()) {
            r = free_.back();
            free_.pop_back();
            in_free_[r] = false;
        } else {
            if (next_ == 255) too_many();
            r = next_++;
        }
        if (r + 1 > max_) max_ = r + 1;
        return r;
    }

    void free(uint8_t r) {
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
    uint8_t alloc_local() {
        uint8_t r = alloc();
        pinned_[r] = true;
        return r;
    }
    // Local scope'tan çıkınca: pin'i kaldır + register'ı havuza iade et.
    void free_local(uint8_t r) {
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
    uint8_t alloc_seq(uint8_t n) {
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
                            [&](uint8_t r) { return r >= base && r < base + n; }),
                        free_.end());
        } else {
            if ((int)next_ + n > 255) too_many();
            base = next_;
            next_ += n;
            if (next_ > max_) max_ = next_;
        }
        for (int k = 0; k < n; ++k) seq_[base + k] = true;
        return (uint8_t)base;
    }

    // alloc_seq bloğunu bırak: koruma kalkar, register'lar havuza döner.
    void release_seq(uint8_t base, uint8_t n) {
        for (int k = 0; k < n; ++k) { seq_[base + k] = false; free((uint8_t)(base + k)); }
    }

    uint8_t max_used() const { return max_; }

private:
    uint8_t locals_end_;
    uint8_t next_;
    uint8_t max_;
    std::vector<uint8_t> free_;      // LIFO (alloc sondan alır)
    bool    in_free_[256] = {false}; // free_ üyeliği (çift free + ardışık blok araması)
    bool    seq_[256]    = {false};  // canlı alloc_seq bloğu (free() atlar)
    bool    pinned_[256] = {false};  // aktif local register'lar (free() atlar)
    std::string owner_;              // hata mesajı için fonksiyon adı
};

// ── LocalVar — lexical scope içindeki değişken ───────────────────────────────

struct LocalVar {
    std::string name;
    uint8_t     reg;
    int         depth;
};

// ── Capture — closure use() listesi ─────────────────────────────────────────

struct CaptureInfo {
    std::string name;
    uint8_t     capture_index; // Closure.captures[] sırası
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
    int  emit(OpCode op, uint8_t a=0, uint8_t b=0, uint8_t c=0);
    int  emit_jump(OpCode op, uint8_t cond_reg=0);  // hedef sonradan patch edilir
    void patch_jump(int offset, int target);
    int  current_ip() const { return (int)proto_.code.size(); }
    // Derlenen statement satiri — emit() proto_.lines'a bunu yazar (VM hata konumu).
    int  cur_line_ = 0;

    // ── Constant pool ──────────────────────────────────────────────────────────
    uint16_t add_const(Value v);
    // Argüman sayısı 8 bit taşınır: 255 üstü SESSİZCE sarıyordu (300 argüman → 44).
    static void check_argc(const CallExpression& e) {
        if (e.arguments.size() > 255)
            throw LookCompileError("a call has more than 255 arguments", e.loc.line);
    }
    // MAKE_CLOSURE: a=r, b/c = 16-bit nested-proto indeksi (b düşük, c yüksek).
    void emit_make_closure(uint8_t r, int fn_idx) {
        emit(OpCode::MAKE_CLOSURE, r, lo8(fn_idx), hi8(fn_idx));
    }
    void     emit_load_const(uint8_t dest, Value v, int line);

    // ── Register ──────────────────────────────────────────────────────────────
    uint8_t alloc_temp();
    void    free_temp(uint8_t r);

    // ── Scope ──────────────────────────────────────────────────────────────────
    void    push_scope();
    void    pop_scope();
    uint8_t declare_local(const std::string& name, int line);

    enum class VarKind { LOCAL, CAPTURE, GLOBAL };
    struct VarLoc { VarKind kind; uint8_t index; };
    VarLoc  resolve_var(const std::string& name, bool for_write = false);

    // ── Yerel / yakalanan erişim yardımcıları ─────────────────────────────────
    // Tüm yerel ve yakalanan değişken erişimi bu üç noktadan geçer.
    void emit_read_local(uint8_t dest, uint8_t slot);   // dest = local(slot)
    void emit_write_local(uint8_t slot, uint8_t src);   // local(slot) = src
    void emit_read_capture(uint8_t dest, uint8_t cap_index); // dest = capture

    // ── Expression → register ─────────────────────────────────────────────────
    // dest=255 → compiler geçici register seçer; caller free_temp() çağırmalı
    uint8_t compile_expr(const Expression& expr, uint8_t dest = 255);

    uint8_t compile_binary(const BinaryExpression& e, uint8_t dest);
    uint8_t compile_logical(const BinaryExpression& e, uint8_t dest); // && ||
    uint8_t compile_call(const CallExpression& e, uint8_t dest);
    uint8_t compile_closure(const FunctionExpression& e, uint8_t dest);
    uint8_t compile_string_interp(const std::string& raw, int line, uint8_t dest);
    uint8_t compile_array_lit(const ArrayLiteral& e, uint8_t dest);
    uint8_t compile_assoc_lit(const AssocArrayLiteral& e, uint8_t dest);
    uint8_t compile_struct_lit(const StructLiteralExpression& e, uint8_t dest);

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
    // CALL_BUILTIN indeks alani 8-bit: 255 ustu SESSIZCE kirpilir → yanlis builtin.
    // builtin_names 255/256 DOLU; yeni giris eklenirse burasi gurultulu hata verir.
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

    // ── Data ──────────────────────────────────────────────────────────────────
    FunctionProto                    proto_;
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

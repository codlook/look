#pragma once

#include "look/ast.h"
#include "look/bytecode.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <stack>
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

// ── RegisterAllocator ─────────────────────────────────────────────────────────
//
// Local değişkenler 0..num_locals-1 arasında sabit slot alır.
// Temp değerler num_locals'tan başlar, free() ile geri verilir.
// Max 255 register — aşılırsa LookCompileError.

class RegisterAllocator {
public:
    explicit RegisterAllocator(uint8_t locals_end)
        : locals_end_(locals_end), next_(locals_end), max_(locals_end) {}

    uint8_t alloc() {
        uint8_t r;
        if (!free_.empty()) {
            r = free_.top();
            free_.pop();
        } else {
            if (next_ == 255)
                throw LookCompileError("Fonksiyon çok karmaşık: 256 register sınırı aşıldı");
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
        if (r >= locals_end_ && !pinned_[r]) free_.push(r);
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

    // Allocate n consecutive registers from next_ (ignores free pool — guarantees contiguity)
    // Also updates locals_end_ so these registers are protected from pool re-use.
    uint8_t alloc_seq(uint8_t n) {
        if (n == 0) return next_;
        if ((int)next_ + n > 255)
            throw LookCompileError("Fonksiyon çok karmaşık: 256 register sınırı aşıldı");
        uint8_t base = next_;
        next_ += n;
        if (next_ > max_) max_ = next_;
        // Protect alloc_seq'd registers: compile_expr returning a local index directly
        // must not contaminate the free pool with these registers.
        if (next_ > locals_end_) locals_end_ = next_;
        return base;
    }

    uint8_t max_used() const { return max_; }

private:
    uint8_t locals_end_;
    uint8_t next_;
    uint8_t max_;
    std::stack<uint8_t> free_;
    bool    pinned_[256] = {false};  // aktif local register'lar (free() atlar)
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
    bool        is_cell = false; // 58: yakalanan değer bir CELL (boxed local) mı →
                                 // closure gövdesi okurken [0] deref etmeli (by-ref)
};

// ── Loop stack — break/continue patch ────────────────────────────────────────

struct LoopContext {
    std::vector<int> break_patches;
    std::vector<int> continue_patches;
    int              continue_target = -1; // loop başı IP
    bool             is_switch = false;    // switch: break'i yakalar, continue'yu
                                           // dıştaki döngüye geçirir (C semantiği)
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

    // ── Local erişim helper'ları (58. bug closure fix hazırlığı) ──────────────
    // Tüm local okuma/yazma bu iki noktadan geçer → "boxed local" (cell) desteği
    // buraya lokalize edilecek. ŞU AN davranış-değişmez: düz MOVE (register).
    void emit_read_local(uint8_t dest, uint8_t slot);   // dest = local(slot) [boxed→cell[0]]
    void emit_write_local(uint8_t slot, uint8_t src);   // local(slot) = src [boxed→cell[0]]
    void emit_read_capture(uint8_t dest, uint8_t cap_index); // dest = capture [cell→[0]]
    bool is_cell_var(const VarLoc& loc) const;          // bu var boxed cell mi

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
    void mark_non_builtin_module_fn() {
        FunctionCompiler* c = this;
        while (c->parent_) c = c->parent_;
        c->non_builtin_module_fn_ = true;
    }
    bool non_builtin_module_fn_ = false;
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
    int                              loop_depth_  = 0; // 58/2c: döngü-body içinde mi
                                                       // (top-level loop-local cell kararı)
    std::set<std::string>            outer_globals_;   // 2c: döngü-DIŞI tanımlı top-level
                                                       // var'lar → döngü-içi reassignment
                                                       // onları cell YAPMAZ (C2 paritesi)
    std::vector<CaptureInfo>         captures_;  // use() listesi

    // ── 58. bug closure fix: escape-analiz (Adım 2a) ──────────────────────────
    // no_discovery_: keşif-geçişinde true → kendi alt-keşfini yapmaz (sonsuz
    //   özyineleme önlenir). escaping_names_: BU fonksiyonun bir closure tarafından
    //   yakalanan local'leri (capture-load sitesinde toplanır). boxed_names_:
    //   keşif-geçişinden gelen, cell'e taşınacak isimler. boxed_slots_: o isimlerin
    //   register slot'ları (declare_local doldurur; Adım 2b helper'da kullanılacak).
    bool                             no_discovery_ = false;
    std::set<std::string>            escaping_names_;
    std::set<std::string>            boxed_names_;
    std::set<uint8_t>                boxed_slots_;

    std::vector<LoopContext>         loop_stack_;  // back() = en iç bağlam

    // iota state — const block içinde
    int                              iota_val_  = 0;
    const Expression*                iota_expr_ = nullptr; // tekrarlanan ifade

    FunctionCompiler*                parent_ = nullptr; // capture için
};

// ── Compiler — public API ─────────────────────────────────────────────────────

class Compiler {
public:
    static CompiledProgram compile(const Program& program);
};

} // namespace look

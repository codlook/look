#pragma once

#include "look/ast.h"
#include "look/bytecode.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <stack>
#include <memory>
#include <stdexcept>

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
        if (r >= locals_end_) free_.push(r);
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
    int              continue_target; // loop başı IP
};

// ── FunctionCompiler — tek fonksiyon/closure için ───────────────────────────

class FunctionCompiler {
public:
    FunctionCompiler(const std::string& name,
                     const std::vector<std::string>& params,
                     bool variadic,
                     FunctionCompiler* parent = nullptr);

    std::shared_ptr<FunctionProto> compile(const BlockStatement& body);
    std::shared_ptr<FunctionProto> compile_stmts(const std::vector<std::unique_ptr<Statement>>& stmts);

private:
    // ── Emit ──────────────────────────────────────────────────────────────────
    int  emit(OpCode op, uint8_t a=0, uint8_t b=0, uint8_t c=0);
    int  emit_jump(OpCode op, uint8_t cond_reg=0);  // hedef sonradan patch edilir
    void patch_jump(int offset, int target);
    int  current_ip() const { return (int)proto_.code.size(); }

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
    VarLoc  resolve_var(const std::string& name);

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
    std::vector<CaptureInfo>         captures_;  // use() listesi

    std::stack<LoopContext>          loop_stack_;

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

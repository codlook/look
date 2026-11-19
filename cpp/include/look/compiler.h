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

// ── Compile-time error ────────────────────────────────────────────────────────

class LookCompileError : public std::runtime_error {
public:
    int line;
    explicit LookCompileError(const std::string& msg, int line = 0)
        : std::runtime_error(msg), line(line) {}
};

// The program's own error (e.g. a pure builtin whose result is unused). In 1.x the CLI wrote this
// error and exited instead of falling back to tree-walk; now every compile error stops
// the program, and the type is preserved only while wrapping.
class LookProgramError : public LookCompileError {
public:
    using LookCompileError::LookCompileError;
};

// ── Checked narrowing ────────────────────────────────────────────────────────
// Bytecode operands are 16 bits (they used to be 8 — a limit of 255 registers/parameters/captures
// and 65535 constants; lifted so that "the app does not start if the VM cannot compile it" never trips).
// Two operands combine to carry a 32-bit field (b<<16|c). EVERY narrowing in the compiler that
// fits a value into an operand goes through here: a compile error if it does not fit. Bare narrowing
// caused four separate silent bugs (the value wrapped at the boundary and the VM ran WRONG without an error).
// Bare narrowing is FORBIDDEN in compiler.cpp — tests/no_unchecked_narrowing.sh checks it in CI.
using Reg = uint16_t;
constexpr Reg NO_REG = 0xFFFF;   // compile_expr(dest): "pick a temp register" (not a valid register)
constexpr long long REG_LIMIT = 0xFFFE;   // highest register index (NO_REG is reserved)
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
// High/low 16 bits of a 32-bit field (b = hi, c = lo — the order varies by instruction).
template <class T> inline uint16_t hi16(T v) { return static_cast<uint16_t>(u32(v, "32-bit operand") >> 16); }
template <class T> inline uint16_t lo16(T v) { return static_cast<uint16_t>(u32(v, "32-bit operand") & 0xFFFF); }
// Capacity hint only (reserve): saturates, carries no meaning.
inline uint16_t hint_u16(size_t n) { return static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n); }
// Signed 16-bit immediate of LOAD_INT.
inline uint16_t i16_bits(long long v) {
    if (v < -32768 || v > 32767) throw LookCompileError("immediate integer out of range");
    return static_cast<uint16_t>(static_cast<int16_t>(v));
}

// ── RegisterAllocator ─────────────────────────────────────────────────────────
//
// Local variables take fixed slots in 0..num_locals-1.
// Temps start at num_locals and are returned with free().
// Max 65534 registers (16-bit operand; 0xFFFF = NO_REG) — LookCompileError if exceeded.

class RegisterAllocator {
public:
    explicit RegisterAllocator(Reg locals_end, std::string owner = "<main>")
        : locals_end_(locals_end), next_(locals_end), max_(locals_end),
          owner_(std::move(owner)) { grow(locals_end); }

    // The register operand is 16 bits → at most 65534 registers per function (in 1.x it was
    // 8 bits / 255, and exceeding it dropped the WHOLE program to the interpreter).
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
        // Pinned (active local) registers do not return to the pool: when compile_expr returns a
        // local's slot directly and the caller does free_temp, that slot
        // would be reused by mistake and corrupt the local (function locals
        // sit in the temp range, so the locals_end_ guard is not enough).
        // A register of a live consecutive block (seq_) does not return either: while compiling arguments
        // compile_expr returns base+k and the caller does free_temp — the block is still in use.
        // (DEFENSIVE: with the seq_ guard disabled, all guards plus nested call/loop/ternary
        // probes were run and none broke — that it is needed was NOT PROVEN; it is kept because the allocator is
        // the riskiest spot.)
        // in_free_: so a double free does not hand out the same register twice.
        if (r >= locals_end_ && !pinned_[r] && !seq_[r] && !in_free_[r]) {
            free_.push_back(r);
            in_free_[r] = true;
        }
    }

    // Allocate a protected local slot — free() does not put it back in the pool (released in pop_scope).
    Reg alloc_local() {
        Reg r = alloc();
        pinned_[r] = true;
        return r;
    }
    // On leaving a local scope: remove the pin + return the register to the pool.
    void free_local(Reg r) {
        pinned_[r] = false;
        free(r);
    }

    // Allocate n consecutive registers (the VM expects call arguments at base+k). Protected by
    // seq_ while the block is live; returned to the pool with release_seq() when done.
    //
    // OLD BUG (register leak): the block was ALWAYS taken from next_ and locals_end_ was
    // raised to protect it. Since locals_end_ never came back down, both the block and all the
    // temporaries allocated before it were lost PERMANENTLY: every call with arguments
    // consumed ~argc+1 registers → at most 72 route() / 63 three-argument calls in one function;
    // beyond that the WHOLE program dropped to the interpreter.
    // Now: first look for a consecutive gap of n in the free pool, otherwise take from next_.
    Reg alloc_seq(Reg n) {
        if (n == 0) return next_;
        int base = -1;
        for (int b = locals_end_; b + n <= next_; ++b) {
            int k = 0;
            while (k < n && in_free_[b + k]) ++k;
            if (k == n) { base = b; break; }
            b += k;   // b+k is not free → continue after it
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

    // Release an alloc_seq block: protection is lifted, registers return to the pool.
    void release_seq(Reg base, Reg n) {
        for (int k = 0; k < n; ++k) { seq_[base + k] = false; free(static_cast<Reg>(base + k)); }
    }

    Reg max_used() const { return max_; }

private:
    // The flag arrays grow with the register count (they used to be fixed 256-element arrays).
    void grow(size_t n) {
        if (n > in_free_.size()) { in_free_.resize(n, 0); seq_.resize(n, 0); pinned_.resize(n, 0); }
    }
    Reg locals_end_;
    Reg next_;
    Reg max_;
    std::vector<Reg>  free_;         // LIFO (alloc takes from the end)
    std::vector<char> in_free_;      // free_ membership (double free + consecutive-block search)
    std::vector<char> seq_;          // live alloc_seq block (free() skips)
    std::vector<char> pinned_;       // active local registers (free() skips)
    std::string owner_;              // function name for the error message
};

// ── LocalVar — a variable inside a lexical scope ─────────────────────────────

struct LocalVar {
    std::string name;
    uint16_t     reg;
    int         depth;
};

// ── Capture — closure use() list ────────────────────────────────────────────

struct CaptureInfo {
    std::string name;
    uint16_t     capture_index; // order in Closure.captures[]
};

// ── Loop stack — break/continue patch ────────────────────────────────────────

struct LoopContext {
    std::vector<int> break_patches;
    std::vector<int> continue_patches;
    int              continue_target = -1; // IP of the loop start
    bool             is_switch = false;    // switch: catches break, passes continue
                                           // to the enclosing loop (C semantics)
    size_t           finally_floor = 0;    // depth of pending_finally_ at loop entry;
                                           // break/continue run the finallys down to this depth
                                           // (try-finally inside the loop), NOT the outer ones (unlike return).
};

// ── FunctionCompiler — for a single function/closure ────────────────────────

class FunctionCompiler {
public:
    FunctionCompiler(const std::string& name,
                     const std::vector<std::string>& params,
                     bool variadic,
                     FunctionCompiler* parent = nullptr);

    std::shared_ptr<FunctionProto> compile(const BlockStatement& body,
        const std::vector<std::unique_ptr<Expression>>* defaults = nullptr);
    std::shared_ptr<FunctionProto> compile_stmts(const std::vector<std::unique_ptr<Statement>>& stmts);

    // Was a non-builtin "mod::fn" call seen in the program? Compiler::compile carries this
    // to CompiledProgram (see the comment in bytecode.h).
    bool used_non_builtin_module_fn() const { return non_builtin_module_fn_; }
    const std::vector<std::string>& non_builtin_module_fn_names() const { return non_builtin_names_; }

private:
    // ── Emit ──────────────────────────────────────────────────────────────────
    int  emit(OpCode op, uint16_t a=0, uint16_t b=0, uint16_t c=0);
    int  emit_jump(OpCode op, uint16_t cond_reg=0);  // target is patched later
    void patch_jump(int offset, int target);
    int  current_ip() const { return (int)proto_.code.size(); }
    // Line of the statement being compiled — emit() writes this to proto_.lines (VM error location).
    int  cur_line_ = 0;

    // ── Constant pool ──────────────────────────────────────────────────────────
    uint32_t add_const(Value v);
    // The argument count fits in 16 bits (in 1.x it was 8: above 255 it silently wrapped, 300 → 44).
    static void check_argc(const CallExpression& e) {
        if ((long long)e.arguments.size() > REG_LIMIT)
            throw LookCompileError("a call has more than 65534 arguments", e.loc.line);
    }
    // MAKE_CLOSURE: a=r, b/c = 32-bit nested-proto index (b low, c high).
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

    // ── Local / captured access helpers ───────────────────────────────────────
    // All local and captured variable access goes through these three points.
    void emit_read_local(uint16_t dest, uint16_t slot);   // dest = local(slot)
    void emit_write_local(uint16_t slot, uint16_t src);   // local(slot) = src
    void emit_read_capture(uint16_t dest, uint16_t cap_index); // dest = capture

    // ── Expression → register ─────────────────────────────────────────────────
    // dest=NO_REG → the compiler picks a temp register; the caller must call free_temp()
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
    // BUG FIX (VM finally-on-return): ACTIVE finally blocks inside try/catch. `return`
    // emits them (innermost to outermost) BEFORE RETURN — otherwise RETURN skips the inline finally
    // (in the interpreter finally ran on return; in the VM it did not → differential divergence).
    std::vector<const BlockStatement*> pending_finally_;
    // Emit the pending finallys up to (not including) floor, innermost to outermost. return→floor=0
    // (all the way to the function); break/continue→the loop's finally_floor (loop-internal only).
    void emit_pending_finallys(size_t floor = 0);
    // Common path for print/write: writes the arguments separated by " ", appends a newline if newline=true.
    void emit_output_args(const std::vector<std::unique_ptr<Expression>>& exprs, bool newline);
    // The CALL_BUILTIN index field (b + next NOP.b) is 32 bits; it used to be 8 bits and anything above 255 was SILENTLY truncated.
    // If it does not fit, this raises a loud error.
    static void check_builtin_index(int bidx, const std::string& name);

    // When a non-builtin "mod::fn" call is seen, mark it on the ROOT compiler (closures
    // are compiled in sub-compilers → walk up to the root via the parent_ chain). Compiler::compile carries this
    // to CompiledProgram; when the CLI sees the flag it errors out without running
    // (instead of crashing at runtime with "not callable").
    void mark_non_builtin_module_fn(const std::string& name) {
        FunctionCompiler* c = this;
        while (c->parent_) c = c->parent_;
        c->non_builtin_module_fn_ = true;
        // The name is kept too: under LOOK_VM_STRICT the CLI can say "which function stops me
        // from using the VM" (the red list = what is to be added to the VM).
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
    bool try_compile_push_assign(const AssignmentExpression& e);   // `$a = array::push($a, v)` / `$a = array::slice($a, 0, -1)` → in place
    void check_no_outer_write(const std::string& name, const std::string& how);   // a function cannot modify what is outside it
    std::set<std::string> outer_reads_;   // names read from outside (global) in this function — may be read but not assigned
    std::map<std::string, int> local_made_at_;   // names newly assigned as locals in this function → line
    [[noreturn]] void throw_outer_read_assign(const std::string& name);   // read → assign
    // Register where the value goes when an element/field assignment is used as an EXPRESSION (-1: not wanted).
    int assign_result_reg_ = -1;

    // ── Data ──────────────────────────────────────────────────────────────────
    FunctionProto                    proto_;
    std::unordered_map<std::string, uint32_t> const_index_;   // add_const dedup (number/text)
    std::unique_ptr<RegisterAllocator> regs_;

    std::vector<LocalVar>            locals_;
    int                              scope_depth_ = 0;
    bool                             callee_ctx_  = false; // a bare name is being compiled as a CALL TARGET
    std::set<std::string>            outer_globals_;   // names assigned at top level (outside any block):
                                                       // if a block assigns to this name it does not
                                                       // create a new local, it writes to the global
    std::vector<CaptureInfo>         captures_;  // use() list + automatically captured

    std::vector<LoopContext>         loop_stack_;  // back() = innermost context

    // iota state — inside a const block
    int                              iota_val_  = 0;
    const Expression*                iota_expr_ = nullptr; // the repeated expression

    FunctionCompiler*                parent_ = nullptr; // for capture
};

// ── Compiler — public API ─────────────────────────────────────────────────────

class Compiler {
public:
    // base_dir: directory of the main script — the root for resolving `use "file.lk"`
    // (file includes) and the sandbox boundary. Empty → a file include is a compile error.
    // main_file: the main script's path (if any) — a `use` back to it is an import cycle.
    static CompiledProgram compile(const Program& program, const std::string& base_dir = "",
                                   const std::string& main_file = "");
    // host_modules: modules the runner itself provides (e.g. `lk test`: assert) — `use <name>`
    // is not rejected, and a call `name::fn(...)` goes to the "name::fn" value the runner put in globals.
    static CompiledProgram compile(const Program& program, const std::string& base_dir,
                                   const std::set<std::string>& host_modules);
};

} // namespace look

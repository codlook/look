// LOOK Bytecode Compiler — AST → FunctionProto
//
// Each FunctionCompiler produces bytecode for one LOOK function/closure.
// Compiler::compile() compiles the whole program.
//
// 3 key design decisions:
//   1. Register allocation: temps are given back with free(), the register limit is enforced
//   2. Short-circuit: && and || are produced as a JUMP_IF_FALSE/TRUE chain
//   3. try/catch: on entering the catch block only the STORE_VARs are valid

#include <unordered_set>
#include "look/read_stream.h"   // read_stream — read the file in one go
#include "look/compiler.h"
#include "look/ast_spine.h"   // binary_left_spine — `$s = $s . a . b` chain handled with a loop
#include "look/param_write_check.h"   // log_param_writes — warnings for included files
#include "look/builtins.h"
#include "look/unused_result.h"   // is_pure_builtin — a pure builtin whose result is discarded
#include "look/runtime_host.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/stack_guard.h"   // look_native_stack_low — an error instead of a crash on a deeply nested expression/block

#include <fstream>
#include <cassert>
#include <sstream>
#include <filesystem>

namespace {
namespace cfs = std::filesystem;
// Context for compiling `use "file.lk"` includes into the same bytecode unit (mirrors
// the UseStatement/module path). Compilation runs single-threaded at load; thread_local
// keeps it isolated per compile. Empty root → a file include is a compile error.
// fiber-ok: compile-time state only; compilation never suspends (files are read synchronously)
thread_local std::string                    g_compile_root;      // main-script dir = sandbox root
thread_local std::vector<std::string>       g_compile_cur_dir;   // dir stack of the file being compiled
thread_local std::unordered_set<std::string> g_compile_included; // dedup / circular guard
thread_local std::vector<std::string>       g_compile_file;      // stack of files being compiled (error location); empty = main script
// fiber-ok: compile-time state only, like the ones above (compilation never suspends).
// Files currently being compiled (including the main script, absolute path) and their display names: a cycle = `use`-ing
// a file that is already on this stack again (Go: import cycle). Separate from g_compile_included: that
// also holds COMPLETED files (the diamond shape — two files using the same file — is allowed).
thread_local std::vector<std::string>       g_compile_stack;
thread_local std::vector<std::string>       g_compile_stack_names;
thread_local std::set<std::string>          g_host_modules;     // modules provided by the runner (lk test: assert)
thread_local std::vector<std::string>       g_use_modules;      // stdlib `use` modules seen → CompiledProgram::use_modules
// User/package modules loaded in this compilation (so a module `use`d twice is compiled once).
// Per compilation: it used to be a list that lived for the whole process — on a hot reload `use package`
// passed through empty, the package's functions vanished ("Undefined variable") and the package's cookie use
// was not visible, so the CSRF check was silently turned off.
// fiber-ok: compile-time state only; compilation never suspends
thread_local std::unordered_set<std::string> g_compile_modules;
// Struct declarations seen in this compilation unit (including included files): name → (signature, line).
thread_local std::unordered_map<std::string, std::pair<std::string, int>> g_compile_structs;
}
#include <stdexcept>
#include <typeinfo>

namespace look {

// ── Builtin index — SINGLE source: look/builtins.h (src/builtins.cpp) ────────
//
// For the CALL_BUILTIN instruction: string name → index.
// The list MOVED from here to src/builtins.cpp — the http_main.cpp wiring is fed from the same
// table, so the two files do not have to be kept in sync by hand
// (the root of the strlen bug was this sync gap).


// ── FunctionCompiler ──────────────────────────────────────────────────────────

FunctionCompiler::FunctionCompiler(const std::string& name,
                                   const std::vector<std::string>& params,
                                   bool variadic,
                                   FunctionCompiler* parent)
    : parent_(parent)
{
    proto_.name     = name;
    proto_.arity    = (int)params.size();
    proto_.variadic = variadic;
    proto_.params   = params;

    // Parameters take the first local slots
    // regs_ is built after the params are done — inside declare_local
    // First build the locals_ list, then the RegisterAllocator
    scope_depth_ = 0;
    // The parameter register index is 16 bits (1.x: 8 bits, above 255 it silently wrapped: 300 parameters → 44).
    if ((long long)params.size() > REG_LIMIT)
        throw LookCompileError("function '" + name + "' has more than 65534 parameters");
    for (auto& p : params) {
        LocalVar lv;
        lv.name  = p;
        lv.reg   = u16(locals_.size(), "parameter count");
        lv.depth = 0;
        locals_.push_back(lv);
    }
    regs_ = std::make_unique<RegisterAllocator>(u16(locals_.size(), "parameter count"),
                                                name.empty() ? std::string("<main>") : name);
}

// ── Emit ──────────────────────────────────────────────────────────────────────

int FunctionCompiler::emit(OpCode op, uint16_t a, uint16_t b, uint16_t c) {
    int ip = (int)proto_.code.size();
    proto_.code.push_back(Instruction::make(op, a, b, c));
    proto_.lines.push_back(cur_line_); // line table — for the VM error location (compile_stmt fills it)
    // File table: one row when the file being compiled changes (entered/left an include).
    static const std::string kMain;
    const std::string& cf = g_compile_file.empty() ? kMain : g_compile_file.back();
    if (proto_.file_runs.empty() ? !cf.empty() : proto_.file_runs.back().second != cf)
        proto_.file_runs.push_back({ip, cf});
    return ip;
}

int FunctionCompiler::emit_jump(OpCode op, uint16_t cond_reg) {
    // Target unknown — b and c fields are 0, filled in later by patch_jump
    return emit(op, cond_reg, 0, 0);
}

void FunctionCompiler::patch_jump(int offset, int target) {
    // 32-bit target: b = hi, c = lo (hi16/lo16 turn a value that does not fit into a compile error;
    // in 1.x it was 16 bits → a loop longer than 65535 instructions failed to compile with "Jump target too far")
    proto_.code[offset].b = hi16(target);
    proto_.code[offset].c = lo16(target);
}

// ── Constant pool ─────────────────────────────────────────────────────────────

uint32_t FunctionCompiler::add_const(Value v) {
    // Deduplicate. Number and text constants use a hash table (it used to scan the whole pool on every
    // constant: trivial while the limit was 65535, but with 70,000 constants compile time grew quadratically).
    // The key is type + exact value: 1 and 1.0 and "1" are separate constants.
    std::string key;
    switch (v.type()) {
        case Value::INT:    key = "i" + std::to_string(v.as_int()); break;
        case Value::FLOAT:  { double d = v.as_float(); key.assign("f"); key.append(reinterpret_cast<const char*>(&d), sizeof d); break; }
        case Value::STRING: key = "s" + v.as_string(); break;
        default: break;
    }
    if (!key.empty()) {
        auto it = const_index_.find(key);
        if (it != const_index_.end()) return it->second;
    } else {
        for (size_t i = 0; i < proto_.constants.size(); ++i)
            if (proto_.constants[i] == v) return u32(i, "constant index");
    }
    const uint32_t idx = u32(proto_.constants.size(), "constant index");
    proto_.constants.push_back(v);
    if (!key.empty()) const_index_.emplace(std::move(key), idx);
    return idx;
}

void FunctionCompiler::emit_load_const(uint16_t dest, Value v, int /*line*/) {
    // Small integer optimization: 16-bit signed range → LOAD_INT (no constant pool)
    if (v.type() == Value::INT && v.as_int() >= -32768 && v.as_int() <= 32767) {
        emit(OpCode::LOAD_INT, dest, i16_bits(v.as_int()));
        return;
    }
    if (v.type() == Value::NONE)  { emit(OpCode::LOAD_NULL,  dest); return; }
    if (v.type() == Value::BOOL) {
        emit(v.as_bool() ? OpCode::LOAD_TRUE : OpCode::LOAD_FALSE, dest);
        return;
    }
    uint32_t idx = add_const(v);
    if (idx <= REG_LIMIT) {
        emit(OpCode::LOAD_CONST,   dest, u16(idx, "constant index"));
    } else {
        emit(OpCode::LOAD_CONST_W, dest, hi16(idx), lo16(idx));
    }
}

// ── Register ──────────────────────────────────────────────────────────────────

uint16_t FunctionCompiler::alloc_temp() {
    return regs_->alloc();
}

void FunctionCompiler::free_temp(uint16_t r) {
    regs_->free(r);
}

// ── Scope ──────────────────────────────────────────────────────────────────────

void FunctionCompiler::push_scope() {
    ++scope_depth_;
}

void FunctionCompiler::pop_scope() {
    // Remove the scope's locals from the list — their registers become free
    while (!locals_.empty() && locals_.back().depth == scope_depth_) {
        regs_->free_local(locals_.back().reg);
        locals_.pop_back();
    }
    --scope_depth_;
}

uint16_t FunctionCompiler::declare_local(const std::string& name, int line) {
    // Is the same name already defined in the same scope?
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name)
            throw LookCompileError("'" + name + "' already defined in this scope", line);
    }
    // New protected local slot — free() does not put it back in the pool (otherwise compile_expr's
    // "return the local's slot directly" optimization + free_temp would corrupt the local).
    uint16_t slot = regs_->alloc_local();
    locals_.push_back({name, slot, scope_depth_});
    return slot;
}

FunctionCompiler::VarLoc FunctionCompiler::resolve_var(const std::string& name, bool for_write) {
    // 1. Local
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it)
        if (it->name == name) return {VarKind::LOCAL, it->reg};

    // 2. Capture (from the use() list OR automatically captured earlier)
    for (auto& c : captures_)
        if (c.name == name) return {VarKind::CAPTURE, c.capture_index};

    // 3. Automatic (implicit) capture — on READ: if the name is a local/capture of the
    //    outer function, capture it into the closure as a by-value snapshot. It used to fall to GLOBAL →
    //    `$m=3; array::map($a, fn($x)=>$x*$m)` could not see $m in the VM and gave 0
    //    (the interpreter saw 3 with lexical scope — a silent divergence). On WRITE
    //    we do not capture: assigning to an outer variable creates a new local (Python-like, the existing
    //    behavior is kept; captures cannot be modified anyway).
    if (!for_write && parent_) {
        VarLoc pl = parent_->resolve_var(name, /*for_write=*/false);
        if (pl.kind != VarKind::GLOBAL) {
            if ((long long)captures_.size() >= REG_LIMIT)
                throw LookCompileError("closure captures more than 65534 variables");
            uint16_t idx = u16(captures_.size(), "capture count");
            captures_.push_back({name, idx});
            return {VarKind::CAPTURE, idx};
        }
    }

    // 4. Global
    return {VarKind::GLOBAL, 0};
}

// ── Local / captured access helpers ───────────────────────────────────────────
// All local and captured variable access goes through these three points: a local is a
// register (MOVE), a captured value is the closure's own copy (LOAD_CAPTURE).
void FunctionCompiler::emit_read_local(uint16_t dest, uint16_t slot) {
    emit(OpCode::MOVE, dest, slot);
}
void FunctionCompiler::emit_write_local(uint16_t slot, uint16_t src) {
    emit(OpCode::MOVE, slot, src);
}
void FunctionCompiler::emit_read_capture(uint16_t dest, uint16_t cap_index) {
    emit(OpCode::LOAD_CAPTURE, dest, cap_index);
}

// ── compile — entry point ─────────────────────────────────────────────────────

std::shared_ptr<FunctionProto> FunctionCompiler::compile(const BlockStatement& body,
        const std::vector<std::unique_ptr<Expression>>* defaults) {
    // (look/capture_check.h): a closure captures by VALUE → no local or
    // parameter is wrapped; all are plain registers.

    // ── Prologue: default parameters ─────────────────────────────────────────
    // If param i was not supplied (argc in the call <= i), fill in its default. Params
    // hold the first local registers (0..arity-1). A default expression may
    // see the earlier params (left to right).
    // Number of required params (argument count check — vm.cpp check_argc).
    {
        const int fixed = proto_.variadic ? proto_.arity - 1 : proto_.arity;
        proto_.required = 0;
        for (int i = 0; i < fixed; ++i)
            if (!(defaults && (size_t)i < defaults->size() && (*defaults)[(size_t)i])) proto_.required = i + 1;
    }
    if (defaults) {
        for (size_t i = 0; i < defaults->size() && i < proto_.arity; ++i) {
            if (!(*defaults)[i]) continue;
            uint16_t ac = alloc_temp();
            emit(OpCode::LOAD_ARGC, ac);
            uint16_t lim = alloc_temp();
            emit_load_const(lim, Value((int64_t)i), 0);
            uint16_t cond = alloc_temp();
            emit(OpCode::LTE, cond, ac, lim);         // argc <= i  → param i was not supplied
            free_temp(ac); free_temp(lim);
            int skip = emit_jump(OpCode::JUMP_IF_FALSE, cond);  // if supplied, skip the default
            free_temp(cond);
            uint16_t dr = compile_expr(*(*defaults)[i]); // the default value
            emit(OpCode::MOVE, u16(i, "parameter index"), dr);          // param i = default (reg i)
            free_temp(dr);
            patch_jump(skip, current_ip());
        }
    }

    compile_block(body);

    // Implicit return null
    emit(OpCode::RETURN_NULL);

    proto_.reg_count = regs_->max_used();
    return std::make_shared<FunctionProto>(std::move(proto_));
}

// ── compile_block ─────────────────────────────────────────────────────────────

void FunctionCompiler::compile_block(const BlockStatement& block) {
    push_scope();
    for (auto& s : block.statements)
        compile_stmt(*s);
    pop_scope();
}

// ── compile_stmt — dispatch ───────────────────────────────────────────────────

// The compiler walks the tree recursively: the parser limits depth (nesting 150, chain 1000) but
// these limits may not be enough on a small stack (the Windows main thread 1 MB, a musl thread 128 KB).
// Long binary chains are compiled with a loop (compile_binary/compile_logical); for the remaining nested forms
// the real remaining stack is checked: a clear error instead of a crash (measured: 1000 terms segfault at 512 KB).
[[noreturn]] static void compile_stack_exhausted(int line) {
    throw LookProgramError((line > 0 ? "line " + std::to_string(line) + ": " : std::string()) +
        "this expression or block is nested too deeply to compile on this thread's stack; "
        "split it into smaller expressions or functions", line);
}

void FunctionCompiler::compile_stmt(const Statement& stmt) {
    if (look_native_stack_low()) compile_stack_exhausted(stmt.loc.line > 0 ? stmt.loc.line : cur_line_);
    // Line tracking: opcodes emitted from here on are written with this statement's line.
    // proto_.lines ALREADY existed but emit() always wrote 0 ("to be added later") → VM
    // errors could not give a line/location (tree-walk gave "File/Line", the VM only the message).
    if (stmt.loc.line > 0) cur_line_ = stmt.loc.line;
    if (auto* s = node_as<ExpressionStatement>(&stmt)) {
        // If the assignment is used as a statement the result is discarded → skip the MOVE that reads
        // the value back. Otherwise compile_expr(Assignment) copies the slot to a temp every time;
        // in an accumulator like `$s .= x` this creates O(n²) (the copy = the whole string).
        if (auto* a = node_as<AssignmentExpression>(s->expression.get())) {
            if (!try_compile_push_assign(*a)) compile_assign_expr(*a);
        } else {
            // A PURE builtin whose result is discarded (array::push($l, $x) on its own): does nothing, a silent
            // bug → the program does not start and the reason is stated (like the rule for a non-existent function).
            if (auto* call = node_as<CallExpression>(s->expression.get())) {
                std::string full;
                if (auto* sr = node_as<ScopeResolution>(call->callee.get()))
                    full = sr->module_name + "::" + sr->member_name;
                else if (auto* v = node_as<Variable>(call->callee.get()))
                    if (is_reserved_builtin(v->name)) full = v->name;   // count(), strtoupper() … (cannot be shadowed)
                if (!full.empty() && is_pure_builtin(full))
                    throw LookProgramError(unused_result_message(full, stmt.loc.line), stmt.loc.line);
            }
            // The result is unused — take a temp register, release it right away
            uint16_t r = compile_expr(*s->expression);
            free_temp(r);
        }
    }
    // print/write semantics (the old interpreter's build_output + PrintStatement rule):
    //   arguments are SEPARATED by " ";  print: APPENDS "\n";  write: does not.
    // It used to call a builtin once per argument here → there was neither a separator nor a newline:
    // print($a,$b) produced "a b\n" in the interpreter and "ab" in the VM (both CLI-VM and
    // web-VM). The differential missed it: the body printed a single argument and $(...) already swallows the
    // trailing newline.
    else if (auto* s = node_as<PrintStatement>(&stmt)) {
        emit_output_args(s->expressions, /*newline=*/true);
    }
    else if (auto* s = node_as<WriteStatement>(&stmt)) {
        emit_output_args(s->expressions, /*newline=*/false);
    }
    else if (auto* s = node_as<ReturnStatement>(&stmt)) {
        compile_return(*s);
    }
    else if (auto* s = node_as<ThrowStatement>(&stmt)) {
        // throw <expr> → compute the value, THROW opcode (caught via try_stack_ / LOAD_EXC)
        uint16_t r = compile_expr(*s->expression);
        emit(OpCode::THROW, r);
        free_temp(r);
    }
    else if (node_as<BreakStatement>(&stmt)) {
        // break goes to the nearest breakable context — a switch or a loop.
        if (loop_stack_.empty())
            throw LookCompileError("break outside a loop/switch");
        // run the try-finallys inside the loop (down to the loop's floor), THEN jump.
        emit_pending_finallys(loop_stack_.back().finally_floor);
        int p = emit_jump(OpCode::JUMP);
        loop_stack_.back().break_patches.push_back(p);
    }
    else if (node_as<ContinueStatement>(&stmt)) {
        // continue SKIPS a switch — it goes to the nearest real LOOP (C semantics).
        int idx = -1;
        for (int i = (int)loop_stack_.size() - 1; i >= 0; --i)
            if (!loop_stack_[i].is_switch) { idx = i; break; }
        if (idx < 0)
            throw LookCompileError("continue outside a loop");
        // run the finallys down to the target loop's floor, THEN jump.
        emit_pending_finallys(loop_stack_[idx].finally_floor);
        int target = loop_stack_[idx].continue_target;
        if (target >= 0) {
            // Target known (while/foreach)
            emit(OpCode::JUMP, 0, hi16(target), lo16(target));
        } else {
            int p = emit_jump(OpCode::JUMP);
            loop_stack_[idx].continue_patches.push_back(p);
        }
    }
    else if (auto* s = node_as<IfStatement>(&stmt)) {
        compile_if(*s);
    }
    else if (auto* s = node_as<WhileStatement>(&stmt)) {
        compile_while(*s);
    }
    else if (auto* s = node_as<ForStatement>(&stmt)) {
        compile_for(*s);
    }
    else if (auto* s = node_as<ForeachStatement>(&stmt)) {
        compile_foreach(*s);
    }
    else if (auto* s = node_as<TryCatchStatement>(&stmt)) {
        compile_try(*s);
    }
    else if (auto* s = node_as<FunctionDeclaration>(&stmt)) {
        compile_func_decl(*s);
    }
    else if (auto* s = node_as<SwitchStatement>(&stmt)) {
        compile_switch(*s);
    }
    else if (auto* s = node_as<ConstBlock>(&stmt)) {
        compile_const_block(*s);
    }
    else if (auto* s = node_as<StructDeclaration>(&stmt)) {
        compile_struct_decl(*s);
    }
    else if (auto* s = node_as<BlockStatement>(&stmt)) {
        compile_block(*s);
    }
    else if (auto* us = node_as<UseStatement>(&stmt)) {
        // `use <name>` covers two different things:
        //   1) a stdlib module (string, jobs, template…) → ALREADY bound in the builtin table;
        //      nothing to do in bytecode → NOP (the old behavior was right).
        //   2) a USER MODULE (~/.look/modules/<name>/<name>.lk) → the file must be loaded and its
        //      functions DEFINED as globals.
        //
        // OLD BUG: both cases produced a NOP. The user module's functions were never created in the
        // VM globals; since the interpreter loaded them the output came out
        // CORRECT, but on the web the route gave "Undefined variable: <fn>" and fell to the interpreter
        // PERMANENTLY. Measured (multi-file project, use model):
        //   [ERROR] VM BUG — route permanently fell to the interpreter (SLOW PATH):
        //           GET:/urun/{id} — Undefined variable: urun_getir
        // That is, the language's "multi-file project with use" path lost its SPEED on the web —
        // and since the result was correct it stayed silent (the fallback masks it).
        //
        // Fix: parse the module file here and fold its statements into the SAME compilation
        // unit. That way the functions become BYTECODE_FN in the VM globals.
        // The distinction is simple: a user module has a FILE, a stdlib module does not.
        bool loaded = false;
        {
#ifdef _WIN32
            const char* home = std::getenv("USERPROFILE");
            if (!home) home = std::getenv("HOMEDRIVE");
#else
            const char* home = std::getenv("HOME");
#endif
            if (home && *home) {
                std::string path = std::string(home) + "/.look/modules/" +
                                   us->module_name + "/" + us->module_name + ".lk";
                // If the same module is `use`d twice in this compilation, do not compile it again — avoid double definitions and bloat.
                static std::vector<std::shared_ptr<Program>> g_module_asts;  // AST lifetime
                if (g_compile_modules.count(path)) { emit(OpCode::NOP); return; }

                std::ifstream f(path);
                if (f) {
                    std::string src = look::read_stream(f);
                    try {
                        Lexer lx(src);
                        Parser p(lx.scan_tokens());
                        auto prog = std::shared_ptr<Program>(p.parse().release());
                        look::log_param_writes(p.warnings(), path);
                        g_compile_modules.insert(path);
                        g_module_asts.push_back(prog);
                        g_compile_file.push_back(path);   // package: outside the application directory → full path
                        struct FilePop { ~FilePop() { g_compile_file.pop_back(); } } file_pop;
                        for (auto& sub : prog->statements) compile_stmt(*sub);
                        loaded = true;
                    } catch (const std::exception& ex) {
                        // No interpreter fallback: a package that fails to compile stops with the reason
                        // (it used to be a NOP: the package's functions were silently missing).
                        const std::string m = ex.what();
                        if (m.rfind("use " + us->module_name + ":", 0) == 0) throw;
                        if (dynamic_cast<const LookProgramError*>(&ex))
                            throw LookProgramError("use " + us->module_name + ": " + m);
                        throw LookCompileError("use " + us->module_name + ": " + m);
                    }
                }
            }
        }
        if (!loaded && g_host_modules.count(us->module_name)) {
            // A module provided by the runner (lk test: `use assert`) — its functions are ready in globals.
            emit(OpCode::NOP);
            return;
        }
        if (!loaded) {
            // Not a package: must be a stdlib module. The two messages of the old interpreter's `use` check —
            // this check used to exist only in the interpreter pass (that pass is gone).
            const std::string& m = us->module_name;
            if (m == "ws" || m == "timer" || m == "sse")
                throw LookCompileError("'" + m + "' is built in and is not a module: remove the 'use "
                    + m + "' line and call " + m + ":: directly.");
            static const std::set<std::string> stdlib_mods = [] {
                std::set<std::string> s;
                for (const auto& n : builtin_names()) {
                    auto c = n.find("::");
                    if (c != std::string::npos) s.insert(n.substr(0, c));
                }
                return s;
            }();
            if (!stdlib_mods.count(m))
                throw LookCompileError("Unknown module: '" + m
                    + "'. If it is a package, install it with: lk module install " + m);
            // The runner opens this after compilation (CompiledProgram::use_modules).
            if (std::find(g_use_modules.begin(), g_use_modules.end(), m) == g_use_modules.end())
                g_use_modules.push_back(m);
            emit(OpCode::NOP);
        }
    }
    else if (auto* ufs = node_as<UseFileStatement>(&stmt)) {
        // `use "file.lk"` — compile the included file's statements into THIS unit so its
        // functions become BYTECODE_FN in VM globals. (Before this, the whole app stayed on
        // the old interpreter fallback — measured ~57× slower on compute: QR/PDF/inflate.) This
        // is the sibling of the module (`use <name>`) path above.
        // Any failure fails the whole compile (see the catch below), and so does a missing base
        // dir: NOP here would wrongly "succeed" with the include's functions missing.
        if (g_compile_root.empty())
            throw LookCompileError("use \"" + ufs->path + "\": file include compiles only with a base directory");
        static std::vector<std::shared_ptr<Program>> g_file_asts;   // AST lifetime (bytecode refs it)
        try {
            cfs::path base = g_compile_cur_dir.empty()
                ? cfs::path(g_compile_root) : cfs::path(g_compile_cur_dir.back());
            std::string abs  = cfs::weakly_canonical(base / ufs->path).string();
            std::string root = cfs::weakly_canonical(cfs::path(g_compile_root)).string();
            if (abs.compare(0, root.size(), root) != 0)
                throw LookCompileError("use \"" + ufs->path + "\": the file is outside the application directory");
            // Cycle (Go: "import cycle not allowed"): requesting a file that is being compiled.
            // It used to be that the main script was not on the stack → a.lk → b.lk → a.lk compiled and ran
            // the main file a SECOND time ("a b a"); a cycle among included files was silently skipped.
            const std::string rel_name = cfs::path(abs).lexically_relative(cfs::path(root)).generic_string();
            for (size_t k = 0; k < g_compile_stack.size(); ++k) {
                if (g_compile_stack[k] != abs) continue;
                std::string chain;
                for (size_t j = k; j < g_compile_stack_names.size(); ++j) chain += g_compile_stack_names[j] + " -> ";
                throw LookProgramError("import cycle: " + chain + rel_name +
                    " (a file cannot use a file that uses it; move the shared code into a third file)",
                    stmt.loc.line);
            }
            if (g_compile_included.count(abs))          { emit(OpCode::NOP); return; }  // diamond: compiled once
            std::ifstream f(abs);
            if (!f) throw LookCompileError("use \"" + ufs->path + "\": the file cannot be opened");
            std::string src = look::read_stream(f);
            g_compile_included.insert(abs);
            Lexer lx(src);
            Parser p(lx.scan_tokens());
            auto prog = std::shared_ptr<Program>(p.parse().release());
            // Parameter-write warnings of the included file (the main file's are
            // logged on the caller side). This used to be done only by the interpreter when loading the file.
            look::log_param_writes(p.warnings(), abs);
            g_file_asts.push_back(prog);
            g_compile_cur_dir.push_back(cfs::path(abs).parent_path().string());
            g_compile_file.push_back(rel_name);
            g_compile_stack.push_back(abs);
            g_compile_stack_names.push_back(rel_name);
            struct DirPop { ~DirPop() {
                g_compile_cur_dir.pop_back(); g_compile_file.pop_back();
                g_compile_stack.pop_back(); g_compile_stack_names.pop_back();
            } } dir_pop;
            for (auto& sub : prog->statements) compile_stmt(*sub);
        } catch (const LookCompileError& ex) {
            // States which file it is in (not repeated if an inner include already named it).
            const std::string m = ex.what();
            if (m.rfind("use \"", 0) == 0) throw;
            // Preserves the program's own error type.
            if (dynamic_cast<const LookProgramError*>(&ex))
                throw LookProgramError("use \"" + ufs->path + "\": " + m, ex.line);
            throw LookCompileError("use \"" + ufs->path + "\": " + m);
        } catch (const std::exception& ex) {
            // OLD BUG: every error was swallowed here and a NOP was written ("the interpreter fallback handles it").
            // Since 1.0.8 the request is not re-run in the interpreter → the included file's
            // functions silently VANISH, compilation is reported "ok" and every route calling them
            // gave a 500 with "Undefined variable". Now compilation fails as a whole:
            // the caller writes the reason and the program does not start.
            throw LookCompileError("use \"" + ufs->path + "\": " + ex.what());
        } catch (...) {
            throw LookCompileError("use \"" + ufs->path + "\": the file could not be compiled");
        }
    }
    else {
        throw LookCompileError("Unknown statement type: " + std::string(typeid(stmt).name()));
    }
}

// ── compile_return ────────────────────────────────────────────────────────────

// A return inside try/catch must run the pending finallys, innermost to outermost, BEFORE RETURN.
// pending_finally_ is cleared temporarily so a return inside a finally does not trigger itself again.
void FunctionCompiler::emit_pending_finallys(size_t floor) {
    if (pending_finally_.size() <= floor) return;
    // Run the [floor..end) suffix innermost to outermost; remove it temporarily so a return/break inside
    // a finally does not trigger itself again, then put it back.
    std::vector<const BlockStatement*> suffix(pending_finally_.begin() + (long)floor, pending_finally_.end());
    pending_finally_.resize(floor);
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
        if (*it) compile_block(**it);
    pending_finally_.insert(pending_finally_.end(), suffix.begin(), suffix.end());
}

void FunctionCompiler::compile_return(const ReturnStatement& s) {
    if (!s.expression) {
        emit_pending_finallys();
        emit(OpCode::RETURN_NULL);
        return;
    }
    uint16_t r = compile_expr(*s.expression);
    // r stays ALLOCATED → the temps of emit_pending_finallys do not overwrite it (temps are above).
    emit_pending_finallys();
    emit(OpCode::RETURN, r);
    free_temp(r);
}

// ── compile_if ────────────────────────────────────────────────────────────────

void FunctionCompiler::compile_if(const IfStatement& s) {
    uint16_t cond = compile_expr(*s.condition);
    int jump_false = emit_jump(OpCode::JUMP_IF_FALSE, cond);
    free_temp(cond);

    compile_block(*s.then_branch);

    if (s.else_branch) {
        int jump_end = emit_jump(OpCode::JUMP);
        patch_jump(jump_false, current_ip());
        compile_block(*s.else_branch);
        patch_jump(jump_end, current_ip());
    } else {
        patch_jump(jump_false, current_ip());
    }
}

// ── compile_while ─────────────────────────────────────────────────────────────

void FunctionCompiler::compile_while(const WhileStatement& s) {
    int loop_start = current_ip();

    uint16_t cond = compile_expr(*s.condition);
    int exit_jump = emit_jump(OpCode::JUMP_IF_FALSE, cond);
    free_temp(cond);

    loop_stack_.push_back({.continue_target = loop_start, .finally_floor = pending_finally_.size()});
    compile_block(*s.body);
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

    // continue → loop_start
    for (int p : ctx.continue_patches) patch_jump(p, loop_start);
    // jump back to the loop
    emit(OpCode::JUMP, 0, hi16(loop_start), lo16(loop_start));

    int after = current_ip();
    patch_jump(exit_jump, after);
    for (int p : ctx.break_patches) patch_jump(p, after);
}

// ── compile_for ───────────────────────────────────────────────────────────────

void FunctionCompiler::compile_for(const ForStatement& s) {
    push_scope();
    if (s.init)      compile_stmt(*s.init);

    int loop_start = current_ip();
    int exit_jump  = -1;
    if (s.condition) {
        uint16_t cond = compile_expr(*s.condition);
        exit_jump = emit_jump(OpCode::JUMP_IF_FALSE, cond);
        free_temp(cond);
    }

    loop_stack_.push_back({.continue_target = -1, .finally_floor = pending_finally_.size()}); // continue target is after post
    compile_block(*s.body);
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

    int post_ip = current_ip();
    if (s.post) {
        uint16_t r = compile_expr(*s.post);
        free_temp(r);
    }

    // continue → post
    for (int p : ctx.continue_patches) patch_jump(p, post_ip);
    emit(OpCode::JUMP, 0, hi16(loop_start), lo16(loop_start));

    int after = current_ip();
    if (exit_jump >= 0) patch_jump(exit_jump, after);
    for (int p : ctx.break_patches) patch_jump(p, after);

    pop_scope();
}

// ── compile_foreach ───────────────────────────────────────────────────────────
//
// FOR_PREP r_iter, r_arr
// loop_start:
// FOR_STEP r_iter, jump_lo, jump_hi → r_iter+2=val, r_iter+3=key
// body...
// JUMP loop_start
// after:

void FunctionCompiler::compile_foreach(const ForeachStatement& s) {
    push_scope();

    uint16_t r_arr = compile_expr(*s.iterable);
    // alloc_seq(4): r_iter, r_iter+1(idx), r_iter+2(val), r_iter+3(key)
    // FOR_STEP writes directly to these 4 registers; alloc_temp()
    // calls in the loop body must not land in this block — alloc_seq guarantees that.
    uint16_t r_iter = regs_->alloc_seq(4);
    emit(OpCode::FOR_PREP, r_iter, r_arr);
    free_temp(r_arr);

    // value and key registers are r_iter+2 and r_iter+3 — FOR_STEP convention
    uint16_t r_val = r_iter + 2;
    uint16_t r_key = r_iter + 3;

    // Loop variables are bound directly to the registers FOR_STEP writes.
    if (!s.value_var.empty()) locals_.push_back({s.value_var, r_val, scope_depth_});
    if (!s.key_var.empty())   locals_.push_back({s.key_var,   r_key, scope_depth_});

    int loop_start = current_ip();
    // FOR_STEP: a=r_iter, b=exit_hi, c=exit_lo (patched later)
    int step_ip = emit(OpCode::FOR_STEP, r_iter, 0, 0);

    loop_stack_.push_back({.continue_target = loop_start, .finally_floor = pending_finally_.size()});
    compile_block(*s.body);
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

    for (int p : ctx.continue_patches) patch_jump(p, loop_start);
    emit(OpCode::JUMP, 0, hi16(loop_start), lo16(loop_start));

    int after = current_ip();
    // FOR_STEP exit patch: write after into the b and c fields
    proto_.code[step_ip].b = hi16(after);
    proto_.code[step_ip].c = lo16(after);

    for (int p : ctx.break_patches) patch_jump(p, after);

    // Obtained with alloc_seq(4) — release all 4 registers at once
    regs_->release_seq(r_iter, 4);
    pop_scope();
}

// ── compile_try ───────────────────────────────────────────────────────────────
//
// Key point 3: on entering the catch block only STORE_VARs are valid.
// Temp registers are already released when the try block ends (normal flow).
// The catch block is a separate scope — the temps inside try have already been given back.

// The builtin index is now 16-BIT: the low 8 bits are in CALL_BUILTIN.b, the high 8 bits in the b field of
// the following NOP hint (bit-compatible: for idx<=255, NOP.b=0 = the old encoding).
// History: while the field was 8-bit, anything above 255 was SILENTLY truncated and the WRONG builtin was called
// (empirically: when the list grew to 285, cache::size index 256 → 0 → called print and returned null).
// This guard now only protects the 16-bit ceiling (65535) — practically unreachable, but it stays so that
// the silent-truncation class can NEVER be silent again.
void FunctionCompiler::check_builtin_index(int bidx, const std::string& name) {
    if (bidx > 65535)
        throw LookCompileError("builtin index exceeded 16-bit ceiling: '" + name + "' index=" +
                               std::to_string(bidx));
}

void FunctionCompiler::emit_output_args(const std::vector<std::unique_ptr<Expression>>& exprs,
                                        bool newline) {
    // builtin 0 (print) and 1 (write) both write the argument RAW (no newline/separator added)
    // → the separator and newline are produced here. The line is built in a TEMPORARY text and
    // written with a SINGLE write (like Go's Println). It used to be a separate write per part: (1) write's return (null)
    // was written into the argument's register → for a local variable print($a) turned $a into null
    // (since the move to the VM; once top level also became registers, for ($i…) { print($i) } broke);
    // (2) the lines of parallel() tasks got mixed together ("1after").
    const uint16_t line = alloc_temp();
    auto append_text = [&](const char* text) {
        uint16_t c = alloc_temp();
        emit_load_const(c, Value(std::string(text)), 0);
        emit(OpCode::CONCAT, line, line, c);     // a == b → append in place
        free_temp(c);
    };
    if (exprs.empty()) emit_load_const(line, Value(std::string()), 0);
    for (size_t i = 0; i < exprs.size(); ++i) {
        uint16_t r = compile_expr(*exprs[i]);
        if (i == 0) emit(OpCode::TO_STR, line, r);
        else { append_text(" "); emit(OpCode::CONCAT, line, line, r); }   // " " between arguments
        free_temp(r);
    }
    if (newline) append_text("\n");              // PrintStatement: "\n" at the end
    emit(OpCode::CALL_BUILTIN, line, 1 /*write*/, line);   // the return goes to a temp register
    free_temp(line);
}

void FunctionCompiler::compile_try(const TryCatchStatement& s) {
    // Make the finally "pending" for the return path: a return inside try+catch runs it first.
    // A separate inline copy is emitted for the normal path (below). Active only while compiling try+catch.
    if (s.finally_block) pending_finally_.push_back(s.finally_block.get());

    // TRY_PUSH: the catch IP will be patched later (b,c = catch_ip)
    int try_push_ip = emit(OpCode::TRY_PUSH, 0, 0, 0);

    compile_block(*s.try_block);

    emit(OpCode::TRY_POP);

    // try succeeded → skip the catch
    int jump_end = emit_jump(OpCode::JUMP);

    // catch block
    int catch_ip = current_ip();
    patch_jump(try_push_ip, catch_ip); // catch_ip into TRY_PUSH's b,c fields

    if (s.catch_block) {
        push_scope();
        // catch variable ($e) — the exception value arrives via LOAD_EXC
        if (!s.catch_var.empty()) {
            uint16_t e_reg = declare_local(s.catch_var, 0);
            emit(OpCode::LOAD_EXC, e_reg);
        }
        compile_block(*s.catch_block);
        pop_scope();
    }

    // Drop the return-path "pending" finally here — try+catch is compiled (a return-in-catch
    // used it). Finally emissions AFTER this are for the normal/throw path, not for return.
    if (s.finally_block && !pending_finally_.empty() && pending_finally_.back() == s.finally_block.get())
        pending_finally_.pop_back();

    if (s.catch_block) {
        // catch CAUGHT → normal flow: the finally (shared) runs, NO re-raise.
        int after_catch = current_ip();
        patch_jump(jump_end, after_catch);
        if (s.finally_block) compile_block(*s.finally_block);
    } else {
        // BUG #2 FIX: NO catch. It used to be that a throw jumped to catch_ip, ran the inline finally and
        // continued → the exception was SILENTLY SWALLOWED. Now: the throw path (at catch_ip)
        // catches the exception with LOAD_EXC, runs the finally, RE-THROWS with THROW
        // (propagates to the outer try/C++). The normal path runs a SEPARATE copy of the finally and continues.
        uint16_t exc = alloc_temp();
        emit(OpCode::LOAD_EXC, exc);                        // original exception (before the finally)
        if (s.finally_block) compile_block(*s.finally_block);  // finally of the throw path
        emit(OpCode::THROW, exc);                           // re-raise
        free_temp(exc);
        int after = current_ip();
        patch_jump(jump_end, after);                        // the normal path lands here
        if (s.finally_block) compile_block(*s.finally_block);  // finally of the normal path (separate copy)
    }
}

// ── compile_func_decl ─────────────────────────────────────────────────────────

void FunctionCompiler::compile_func_decl(const FunctionDeclaration& s) {
    // Shadowing a builtin = error, at definition time (compile error → the program does not start).
    if (is_reserved_builtin(s.name))
        throw std::runtime_error("'" + s.name + "' is a builtin — cannot be redefined");
    // Compile the inner function
    FunctionCompiler inner(s.name, s.parameters, s.is_variadic, this);
    auto proto = inner.compile(*s.body, &s.defaults);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    // The function name is registered in the global scope
    uint32_t name_idx = add_const(Value(s.name));
    uint16_t  r        = alloc_temp();

    // If the inner function captures the outer locals/captures (implicit capture —
    // collected during inner.compile), do the SAME capture setup as compile_closure.
    // It used to be that this block did NOT EXIST → the LOAD_CAPTUREs in the proto body CRASHED at runtime
    // with "Capture index out of range" (named nested fn capture; the VM crashed while tree-walk worked).
    // Capture loading is right before MAKE_CLOSURE; the hints right after.
    std::vector<uint16_t> cap_regs;
    for (auto& cap : inner.captures_) {
        auto loc = resolve_var(cap.name);
        uint16_t cr = alloc_temp();
        if      (loc.kind == VarKind::LOCAL)   emit(OpCode::MOVE, cr, loc.index);
        else if (loc.kind == VarKind::CAPTURE) emit(OpCode::LOAD_CAPTURE, cr, loc.index);
        else { uint32_t ni = add_const(Value(cap.name)); emit(OpCode::LOAD_GLOBAL, cr, hi16(ni), lo16(ni)); }
        cap_regs.push_back(cr);
    }
    // MAKE_CLOSURE: a=r, b=fn_idx (nested index)
    emit_make_closure(r, fn_idx);
    for (uint16_t cr : cap_regs) { emit(OpCode::LOAD_CAPTURE, 0, cr); free_temp(cr); }
    // Register as a global — top-level function declaration (16-bit const index)
    emit(OpCode::STORE_GLOBAL, r, hi16(name_idx), lo16(name_idx));
    free_temp(r);
}

// ── compile_switch ────────────────────────────────────────────────────────────
//
// Go style — every case breaks automatically, no fallthrough.

void FunctionCompiler::compile_switch(const SwitchStatement& s) {
    uint16_t subj = compile_expr(*s.subject);

    std::vector<int> case_jumps;   // jump to the start of each case
    std::vector<int> end_jumps;    // jump out of the switch at the end of each case

    // For each case: check the condition → if no match, jump to the next case
    std::vector<int> body_ips;
    std::vector<int> default_ip_holder; // IP of the default case

    // First pass: condition checks
    struct CaseEntry { std::vector<int> cond_jumps; bool is_default; };
    std::vector<CaseEntry> entries;

    for (auto& c : s.cases) {
        CaseEntry entry;
        entry.is_default = c.values.empty();
        if (!entry.is_default) {
            for (auto& v : c.values) {
                uint16_t r = compile_expr(*v);
                uint16_t eq = alloc_temp();
                emit(OpCode::EQ, eq, subj, r);
                free_temp(r);
                // EQ true → enter this case
                entry.cond_jumps.push_back(emit_jump(OpCode::JUMP_IF_TRUE, eq));
                free_temp(eq);
            }
        }
        entries.push_back(std::move(entry));
    }

    // No case matched → default or the end
    int no_match_jump = emit_jump(OpCode::JUMP);

    // switch context: a `break` inside a case jumps to the end of the switch (NOT to the
    // enclosing loop). is_switch=true → continue skips this context and goes to the enclosing loop.
    loop_stack_.push_back({.is_switch = true, .finally_floor = pending_finally_.size()});

    // Second pass: bodies
    for (size_t i = 0; i < s.cases.size(); ++i) {
        auto& c    = s.cases[i];
        auto& entry = entries[i];
        int body_ip = current_ip();

        if (entry.is_default) {
            // default case: no_match_jump arrives here
            patch_jump(no_match_jump, body_ip);
        } else {
            for (int j : entry.cond_jumps) patch_jump(j, body_ip);
        }

        push_scope();
        for (auto& stmt : c.body) compile_stmt(*stmt);
        pop_scope();
        end_jumps.push_back(emit_jump(OpCode::JUMP));
    }

    auto sctx = loop_stack_.back();
    loop_stack_.pop_back();

    int after = current_ip();

    // without a default, no_match_jump → after
    bool has_default = false;
    for (auto& e : entries) if (e.is_default) { has_default = true; break; }
    if (!has_default) patch_jump(no_match_jump, after);

    for (int p : end_jumps)          patch_jump(p, after);
    for (int p : sctx.break_patches) patch_jump(p, after);  // break → end of the switch
    free_temp(subj);
}

// ── compile_const_block ───────────────────────────────────────────────────────

void FunctionCompiler::compile_const_block(const ConstBlock& s) {
    iota_val_ = 0;
    // iota_expr_: the first expression containing iota — later lines repeat the same expression
    const Expression* last_iota_expr = nullptr;

    for (auto& item : s.items) {
        Value v;
        if (!item.value) {
            // Re-evaluate the previous iota expression (the iota value has advanced)
            // Compiler simplification: iota_ is linear → support numeric only
            v = Value(iota_val_++);
        } else if (auto* iota = node_as<IotaExpression>(item.value.get())) {
            v = Value(iota_val_++);
            last_iota_expr = nullptr;
        } else {
            // Constant expression — attempt compile-time evaluation
            // Take the value directly for simple literals
            if (auto* n = node_as<NumberLiteral>(item.value.get())) {
                v = Value(n->value);
            } else if (auto* f = node_as<FloatLiteral>(item.value.get())) {
                v = Value(f->value);
            } else if (auto* str = node_as<StringLiteral>(item.value.get())) {
                v = Value(str->value);
            } else if (auto* b = node_as<BooleanLiteral>(item.value.get())) {
                v = Value(b->value);
            } else {
                // Complex expression: evaluate at runtime, store to the global
                uint16_t r = compile_expr(*item.value);
                uint32_t name_idx = add_const(Value(item.name));
                emit(OpCode::STORE_GLOBAL, r, hi16(name_idx), lo16(name_idx));
                free_temp(r);
                continue;
            }
        }
        // Load the constant value into the global
        uint16_t r = alloc_temp();
        emit_load_const(r, v, 0);
        uint32_t name_idx = add_const(Value(item.name));
        emit(OpCode::STORE_GLOBAL, r, hi16(name_idx), lo16(name_idx));
        free_temp(r);
    }
}

// ── compile_struct_decl ───────────────────────────────────────────────────────

void FunctionCompiler::compile_struct_decl(const StructDeclaration& s) {
    // 1.x linking fix (2026-09-30): the VM did NOT know the struct definition AT ALL — the old
    // code emitted a fake NEW_STRUCT at the declaration and dropped the field names, never compiled the default
    // expressions; and no code ever assigned the VM's shared_.struct_defs pointer.
    // Result in the VM (CLI + web production path): defaults were NOT APPLIED and
    // a literal with an unknown field was silently accepted. Now the definition is written to a hidden global
    // at the declaration's OWN position (a dynamic default like `a: $d` is correct):
    //   "__sdef:Name" = [f1, d1, f2, d2, ...]   (a field with no default → null)
    // On the CLI a single VM runs it; on the web the setup VM runs it and vm_setup_globals carries it to
    // every request's VM. ':' is invalid in an identifier → it cannot collide with a user name.
    // Triples [name, default, type] — type "" = untyped (rules in look/struct_types.h).
    // A DIFFERENT second declaration with the same name = error; an identical one is allowed. The parser catches the case
    // within the same file; this is the one across included files (the interpreter caught it at load,
    // the VM never looked — it showed up once the command line started compiling included files).
    {
        auto seen = g_compile_structs.find(s.name);
        if (seen != g_compile_structs.end() && seen->second.first != s.signature)
            throw LookCompileError("struct '" + s.name + "' is already declared at line "
                + std::to_string(seen->second.second) + " with different fields");
        g_compile_structs.emplace(s.name, std::make_pair(s.signature, s.decl_line));
    }
    uint16_t arr = alloc_temp();
    emit(OpCode::NEW_ARRAY, arr, hint_u16(s.fields.size() * 3));
    for (auto& f : s.fields) {
        uint16_t t = alloc_temp();
        emit_load_const(t, Value(f.name), 0);
        emit(OpCode::ARRAY_PUSH, arr, t);
        if (f.default_expr) {
            uint16_t dv = compile_expr(*f.default_expr, t);
            if (dv != t) emit(OpCode::MOVE, t, dv);
        } else {
            emit(OpCode::LOAD_NULL, t);
        }
        emit(OpCode::ARRAY_PUSH, arr, t);
        emit_load_const(t, Value(f.type), 0);
        emit(OpCode::ARRAY_PUSH, arr, t);
        free_temp(t);
    }
    uint32_t gi = add_const(Value("__sdef:" + s.name));
    emit(OpCode::STORE_GLOBAL, arr, hi16(gi), lo16(gi));
    free_temp(arr);
}

// ── compile_assign_expr ───────────────────────────────────────────────────────

// The `$a = array::push($a, v)` statement: array::push copies the whole array on each call → quadratic in a loop
// (20k elements × 320 requests: 257 s, `$a[$i] = v` 6 s). As a statement, if the target is the SAME plain
// variable as the first argument, the result is the same as `push($a, v)`: $a's storage grows in place (Go append), and if shared
// it is copied once (copy-on-write) — no other variable is affected.
// Condition: $a MUST NOT CHANGE while the value is computed — in the old order the append goes to the array read BEFORE the value,
// with in-place append to the current $a. Anything that can change $a directly stays out in every case:
// assignment, ++/--, the core push()/pop() (modify the variable in place) — anywhere in the
// expression. A function call is allowed: no function can change a variable outside itself —
// another's locals are invisible, a closure captures by value, writing in place to a top-level variable is a compile
// error (check_no_outer_write; a function used to be able to change a global, and then a call at global
// level was outside). tests/array_push_assign.lk and tests/outer_write_test.sh pin these assumptions;
// they break if the language gains a reference parameter / mutable capture.
// Closure bodies are not descended into (their own locals).
// Resolution and read order are the same as before (write resolution first, then read resolution; the global is read first)
// → errors in the same order, with the same message.
static bool push_value_is_pure(const Expression& x) {
    switch (x.kind) {
        case NodeKind::NumberLiteral: case NodeKind::FloatLiteral: case NodeKind::BooleanLiteral:
        case NodeKind::NullLiteral: case NodeKind::Variable: case NodeKind::ScopeResolution:
        case NodeKind::IotaExpression: case NodeKind::FunctionExpression:
            return true;
        case NodeKind::StringLiteral: {
            // The {$…} part is parsed at compile time: any text that could contain assignment/++/--/push/pop goes to the old
            // path — a conservative look at the text.
            const auto& s = static_cast<const StringLiteral&>(x);
            if (s.raw || s.value.find("{$") == std::string::npos) return true;
            const std::string& v = s.value;
            return v.find('=') == std::string::npos && v.find("++") == std::string::npos &&
                   v.find("--") == std::string::npos && v.find("push") == std::string::npos &&
                   v.find("pop") == std::string::npos;
        }
        case NodeKind::CallExpression: {
            const auto& c = static_cast<const CallExpression&>(x);
            if (auto* v = node_as<Variable>(c.callee.get()))
                if (v->name == "push" || v->name == "pop") return false;   // changes the VARIABLE of the first argument
            if (!c.callee || !push_value_is_pure(*c.callee)) return false;
            for (const auto& a : c.arguments)
                if (!a || !push_value_is_pure(*a)) return false;
            return true;
        }
        case NodeKind::StructLiteralExpression: {
            for (const auto& f : static_cast<const StructLiteralExpression&>(x).fields)
                if (!f.second || !push_value_is_pure(*f.second)) return false;
            return true;
        }
        case NodeKind::ArrayLiteral: {
            for (const auto& el : static_cast<const ArrayLiteral&>(x).elements)
                if (!el || !push_value_is_pure(*el)) return false;
            return true;
        }
        case NodeKind::AssocArrayLiteral: {
            for (const auto& kv : static_cast<const AssocArrayLiteral&>(x).pairs)
                if (!kv.first || !kv.second || !push_value_is_pure(*kv.first) ||
                    !push_value_is_pure(*kv.second))
                    return false;
            return true;
        }
        case NodeKind::IndexExpression: {
            const auto& ix = static_cast<const IndexExpression&>(x);
            return ix.object && ix.index && push_value_is_pure(*ix.object) &&
                   push_value_is_pure(*ix.index);
        }
        case NodeKind::MemberAccessExpression: {
            const auto& ma = static_cast<const MemberAccessExpression&>(x);
            return ma.object && push_value_is_pure(*ma.object);
        }
        case NodeKind::UnaryExpression: {
            const auto& u = static_cast<const UnaryExpression&>(x);
            return u.op != "++" && u.op != "--" && u.right && push_value_is_pure(*u.right);
        }
        case NodeKind::BinaryExpression: {
            const auto& b = static_cast<const BinaryExpression&>(x);
            return b.left && b.right && push_value_is_pure(*b.left) &&
                   push_value_is_pure(*b.right);
        }
        case NodeKind::TernaryExpression: {
            const auto& t = static_cast<const TernaryExpression&>(x);
            return t.condition && t.then_expr && t.else_expr && push_value_is_pure(*t.condition) &&
                   push_value_is_pure(*t.then_expr) && push_value_is_pure(*t.else_expr);
        }
        default:
            return false;   // assignment and every unknown node → the old path
    }
}

// Is it a constant integer? `-1` comes from the parser as unary minus + 1.
static bool is_int_literal(const Expression* x, int64_t v) {
    if (!x) return false;
    if (auto* n = node_as<NumberLiteral>(x)) return n->value == v;
    if (auto* u = node_as<UnaryExpression>(x); u && u->op == "-" && u->prefix && u->right)
        if (auto* n = node_as<NumberLiteral>(u->right.get())) return v != INT64_MIN && n->value == -v;
    return false;
}

// `$a = array::push($a, v)` → in-place append (PUSH_PATH); `$a = array::slice($a, 0, -1)` (constants 0
// and -1) → in-place drop of the last element (SHRINK_PATH). The old path copied the whole list on every step
// (quadratic in a loop). The result and errors are the same as the call's (the VM side gives the same messages).
bool FunctionCompiler::try_compile_push_assign(const AssignmentExpression& e) {
    if (e.op != "=" || e.index || e.object || !e.value) return false;
    auto* call = node_as<CallExpression>(e.value.get());
    if (!call) return false;
    auto* sr = node_as<ScopeResolution>(call->callee.get());
    if (!sr || sr->module_name != "array") return false;
    const bool is_push = sr->member_name == "push" && call->arguments.size() == 2;
    const bool is_shrink = sr->member_name == "slice" && call->arguments.size() == 3 &&
                           is_int_literal(call->arguments[1].get(), 0) &&
                           is_int_literal(call->arguments[2].get(), -1);
    if (!is_push && !is_shrink) return false;
    if (builtin_index(is_push ? "array::push" : "array::slice") < 0) return false;
    auto* first = node_as<Variable>(call->arguments[0].get());
    if (!first || first->name != e.name) return false;
    if (is_push && (!call->arguments[1] || !push_value_is_pure(*call->arguments[1]))) return false;
    // Same order as compile_assign_expr: write resolution first, then (while compiling the value) the read.
    const auto wloc = resolve_var(e.name, /*for_write=*/true);
    const auto rloc = resolve_var(e.name, /*for_write=*/false);
    uint16_t kind; uint32_t ix;
    if (wloc.kind == VarKind::LOCAL && rloc.kind == VarKind::LOCAL && wloc.index == rloc.index) {
        kind = 0; ix = rloc.index;
    } else if (!parent_ && wloc.kind == VarKind::GLOBAL && rloc.kind == VarKind::GLOBAL) {
        // Only the script's OWN top level. Inside a function the write resolution also returns GLOBAL but
        // the assignment creates a new LOCAL there: the fast path would append to the global (a bug between 2ca4aea–7dd626f:
        // `function f() { $a = array::push($a, 2) }` grew the top-level $a).
        // The old path reads $a BEFORE the value (if undefined, that error comes first): do the same read, then release the register
        // (a second owner left in the temp register would force the in-place growth into a copy).
        uint16_t t = compile_expr(*call->arguments[0]);
        emit(OpCode::LOAD_NULL, t);
        free_temp(t);
        kind = 4; ix = add_const(Value(e.name));
    } else {
        return false;   // capture / shadowing: the old path (with its own error and meaning)
    }
    if (is_shrink) {
        emit(OpCode::SHRINK_PATH, 0, 0, 0);
        emit(OpCode::NOP, kind, hi16(ix), lo16(ix));
        return true;
    }
    uint16_t val = compile_expr(*call->arguments[1]);
    uint16_t r = alloc_temp();
    emit(OpCode::PUSH_PATH, r, 0, 0);
    emit(OpCode::NOP, kind, hi16(ix), lo16(ix));
    emit(OpCode::NOP, val, 1);   // b=1: if not an array, array::push's own error message
    free_temp(val);
    free_temp(r);
    return true;
}

// A function (named or closure; including those in files brought in by `use`) cannot modify a variable
// outside itself IN PLACE — the function counterpart of the closure capture rule (capture_check.h). It used to be that `$t[k] = …`,
// `$t.f = …`, `$t[k] += …`, `$t[k]++`, push/pop($t…), `$t++`
// silently modified the top-level $t, while plain `$t = …` and `$t += …` created a local
// (docs: "function scope is isolated"). Reading is free. A compile error: the program does not start.
// The scope was derived from the VM: the paths that write to a variable in place are SET_PATH / PUSH_PATH / POP_PATH (root
// GLOBAL) and ++/-- (STORE_GLOBAL); it does not change the storage of another builtin's argument.
// The suggestion is NOT app::set(): that registry is shared by all requests (per-request data would leak).
void FunctionCompiler::check_no_outer_write(const std::string& name, const std::string& how) {
    if (!parent_) return;   // the top level itself
    const std::string bare = (!name.empty() && name[0] == '$') ? name.substr(1) : name;
    const std::string who = proto_.name == "<closure>" ? "closure" : "function " + proto_.name + "()";
    // The line goes in the message (like unused_result_message): on a compile error the command line prints only the file.
    const std::string at = cur_line_ > 0 ? "line " + std::to_string(cur_line_) + ": " : "";
    throw LookProgramError(at + who + " changes $" + bare + ", a variable outside it (" + how + "); a function "
        "cannot change variables outside it — pass the value as a parameter and return the new value",
        cur_line_);
}

// In a function the same name is first read from outside and then assigned as a local (Python
// UnboundLocalError). The reverse order (reading after a local whose block has ended) has a separate message in the Variable read.
void FunctionCompiler::throw_outer_read_assign(const std::string& name) {
    const std::string bare = (!name.empty() && name[0] == '$') ? name.substr(1) : name;
    const std::string who = proto_.name == "<closure>" ? "closure" : "function " + proto_.name + "()";
    const std::string at = cur_line_ > 0 ? "line " + std::to_string(cur_line_) + ": " : "";
    throw LookProgramError(at + who + " reads $" + bare + " from outside it and also assigns $" + bare
        + "; the assignment makes a new local and leaves the outer $" + bare + " unchanged — pass the "
        "value as a parameter and return the new value, or use another name", cur_line_);
}

void FunctionCompiler::compile_assign_expr(const AssignmentExpression& e) {
    // Plain `$x = v` write target (NO auto-capture → assigning to an outer variable creates a new
    // local). With `$arr[i]=v`, the $arr container is READ and mutated in place →
    // auto-capture is needed (the reference type is shared).
    const int want = assign_result_reg_;   // whether the expression value is wanted (inner assignments set up their own request)
    assign_result_reg_ = -1;
    auto loc = resolve_var(e.name, /*for_write=*/(e.index == nullptr));

    if (e.index) {
        // $arr[i] = val  ·  chained $l.s.x = v / $m["rows"][0] = v
        // (value semantics): the target is compiled as a PATH — root variable + keys —
        // and SET_PATH does copy-on-write level by level in the root's own slot. It used to load the container
        // into a temp register and modify it in place with ARRAY_SET (reference semantics).
        std::vector<const Expression*> key_exprs;      // in order, outermost to innermost
        std::vector<std::string>       key_fields;     // field name when key_exprs[i] == nullptr
        const Variable* root = nullptr;
        bool is_path = true;
        {
            std::vector<std::pair<const Expression*, std::string>> rev;
            const Expression* cur = e.object.get();
            while (cur) {
                if (auto* ix = node_as<IndexExpression>(cur)) {
                    rev.push_back({ix->index.get(), ""}); cur = ix->object.get();
                } else if (auto* ma = node_as<MemberAccessExpression>(cur)) {
                    rev.push_back({nullptr, ma->field}); cur = ma->object.get();
                } else if (auto* v = node_as<Variable>(cur)) {
                    root = v; break;
                } else { is_path = false; break; }       // like f()[0] = x: the root is not a variable
            }
            for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
                key_exprs.push_back(it->first); key_fields.push_back(it->second);
            }
            key_exprs.push_back(e.index.get()); key_fields.push_back("");
        }
        if (is_path && key_exprs.size() <= 250) {
            const std::string& root_name = root ? root->name : e.name;
            auto rloc = resolve_var(root_name, /*for_write=*/false);
            if (rloc.kind == VarKind::GLOBAL)
                // `$t[k]++` becomes `+= 1` in the parser: do not name an operator the user did not write.
                check_no_outer_write(root_name, e.op == "=" ? "an element or field of it is assigned"
                                                            : "an element or field of it is changed");
            const uint16_t n = u16(key_exprs.size(), "index path length");
            uint16_t base = regs_->alloc_seq(n);
            for (uint16_t k = 0; k < n; ++k) {
                const uint16_t kr = u16(base + k, "register index");
                if (key_exprs[k]) {
                    uint16_t ev = compile_expr(*key_exprs[k], kr);
                    if (ev != kr) emit(OpCode::MOVE, kr, ev);
                } else {
                    emit_load_const(kr, Value(key_fields[k]), cur_line_);
                }
            }
            uint16_t val = compile_expr(*e.value);
            if (e.op != "=") {
                // Compound (+= -= …): read the current value with the SAME keys, combine, then write.
                static const std::unordered_map<std::string, OpCode> COMPOUND = {
                    {"+=", OpCode::ADD},  {"-=", OpCode::SUB},  {"*=", OpCode::MUL},
                    {"/=", OpCode::DIV},  {"%=", OpCode::MOD},  {".=", OpCode::CONCAT},
                    {"&=", OpCode::BAND}, {"|=", OpCode::BOR},  {"^=", OpCode::BXOR},
                };
                auto it = COMPOUND.find(e.op);
                if (it == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
                uint16_t cur = alloc_temp();
                if      (rloc.kind == VarKind::LOCAL)   emit_read_local(cur, rloc.index);
                else if (rloc.kind == VarKind::CAPTURE) emit_read_capture(cur, rloc.index);
                else {
                    uint32_t ni = add_const(Value(root_name));
                    emit(OpCode::LOAD_GLOBAL, cur, hi16(ni), lo16(ni));
                }
                for (uint16_t k = 0; k < n; ++k)
                    emit(OpCode::ARRAY_GET, cur, cur, u16(base + k, "register index"));
                if (want >= 0 && e.yield_old) emit(OpCode::MOVE, u16(want, "register index"), cur);   // postfix ++/--: the old value
                uint16_t res = alloc_temp();
                emit(it->second, res, cur, val);
                free_temp(cur); free_temp(val);
                val = res;
            }
            // Root: 0 local · 4 global. A captured variable cannot be modified.
            uint16_t kind; uint32_t ix;
            if (rloc.kind == VarKind::LOCAL) {
                kind = 0; ix = rloc.index;
            } else if (rloc.kind == VarKind::CAPTURE) {
                throw LookCompileError("Captured variable cannot be changed: $" + root_name);
            } else {
                kind = 4; ix = add_const(Value(root_name));
                if (parent_ == nullptr && scope_depth_ <= 1) outer_globals_.insert(root_name);   // a global born through a path at top level
            }
            emit(OpCode::SET_PATH, val, base, n);
            emit(OpCode::NOP, kind, hi16(ix), lo16(ix));
            if (want >= 0 && !e.yield_old) {
                // The value of the expression is the WRITTEN value (as converted to the field type) — it is read
                // back with the same keys; the keys are not recomputed. It used to return the WHOLE root variable.
                const uint16_t w = u16(want, "register index");
                if (rloc.kind == VarKind::LOCAL) emit_read_local(w, rloc.index);
                else { uint32_t ni = add_const(Value(root_name)); emit(OpCode::LOAD_GLOBAL, w, hi16(ni), lo16(ni)); }
                for (uint16_t k = 0; k < n; ++k) emit(OpCode::ARRAY_GET, w, w, u16(base + k, "register index"));
            }
            free_temp(val);
            regs_->release_seq(base, n);
            return;
        }
        // A target whose root is not a variable (writing to a temporary value): the old path.
        uint16_t arr = compile_expr(*e.object);
        uint16_t idx = compile_expr(*e.index);
        uint16_t val = compile_expr(*e.value);
        if (e.op != "=") {
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD},  {"-=", OpCode::SUB},  {"*=", OpCode::MUL},
                {"/=", OpCode::DIV},  {"%=", OpCode::MOD},  {".=", OpCode::CONCAT},
                {"&=", OpCode::BAND}, {"|=", OpCode::BOR},  {"^=", OpCode::BXOR},
            };
            auto it = COMPOUND.find(e.op);
            if (it == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
            uint16_t cur = alloc_temp();
            emit(OpCode::ARRAY_GET, cur, arr, idx);
            if (want >= 0 && e.yield_old) emit(OpCode::MOVE, u16(want, "register index"), cur);
            uint16_t res = alloc_temp();
            emit(it->second, res, cur, val);
            free_temp(cur); free_temp(val);
            val = res;
        }
        emit(OpCode::ARRAY_SET, arr, idx, val);
        if (want >= 0 && !e.yield_old) emit(OpCode::ARRAY_GET, u16(want, "register index"), arr, idx);
        free_temp(val); free_temp(idx); free_temp(arr);
        return;
    }

    // `$s = $s . a . b` — by definition `$s .= a . b`: appended to the target IN PLACE. OLD: the right side was built
    // from scratch in a temp register and written → a copy of the whole text at every step, quadratic in a loop (measured:
    // 100k appends inside a function 175 ms, 1 million with `.=` 33 ms). The operands are FIRST evaluated in order
    // (all see the old $s, the original order), then appended in order; if an operand is the target
    // itself an instant copy is taken (a shared value is copied once on append — same result).
    // Only for a local and a top-level global target; chain ≤ 16 operands (register pressure).
    const bool global_target = loc.kind != VarKind::LOCAL && loc.kind != VarKind::CAPTURE &&
        !(parent_ != nullptr || (scope_depth_ > 1 && !outer_globals_.count(e.name)));
    if (e.op == "=" && (loc.kind == VarKind::LOCAL || global_target)) {
        if (auto* b = node_as<BinaryExpression>(e.value.get())) {
            std::vector<const BinaryExpression*> spine;
            const Expression* first = binary_left_spine(b, spine);
            bool all_concat = spine.size() <= 16;
            for (auto* x : spine) if (x->op != "." && x->op != "..") { all_concat = false; break; }
            auto* fv = node_as<Variable>(first);
            if (all_concat && fv && fv->name == e.name) {
                std::vector<uint16_t> parts;
                for (size_t k = spine.size(); k-- > 0; ) {           // innermost to outermost = left to right
                    uint16_t r = compile_expr(*spine[k]->right);
                    if (loc.kind == VarKind::LOCAL && r == loc.index) {   // instant copy of the target
                        uint16_t t = alloc_temp();
                        emit(OpCode::MOVE, t, r);
                        r = t;
                    }
                    parts.push_back(r);
                }
                uint32_t ni = 0;
                if (global_target) { outer_globals_.insert(e.name); ni = add_const(Value(e.name)); }
                for (uint16_t r : parts) {
                    if (global_target) emit(OpCode::APPEND_GLOBAL, r, hi16(ni), lo16(ni));
                    else               emit(OpCode::CONCAT, loc.index, loc.index, r);
                }
                for (size_t k = parts.size(); k-- > 0; ) free_temp(parts[k]);
                return;
            }
        }
    }

    uint16_t val = compile_expr(*e.value);

    if (loc.kind == VarKind::LOCAL) {
        // B7: `$s .= x` → in-place CONCAT (dst==b==slot). Amortized O(1) with the VM's append_in_place;
        // the cur/tmp MOVEs (two O(n) copies) are also eliminated.
        if (e.op == ".=") {
            emit(OpCode::CONCAT, loc.index, loc.index, val);
            free_temp(val);
        }
        // For compound assign, read the current value
        else if (e.op != "=") {
            uint16_t cur = alloc_temp();
            emit_read_local(cur, loc.index);
            uint16_t tmp = alloc_temp();
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD}, {"-=", OpCode::SUB}, {"*=", OpCode::MUL},
                {"/=", OpCode::DIV}, {"%=", OpCode::MOD}, {".=", OpCode::CONCAT},
                {"&=", OpCode::BAND}, {"|=", OpCode::BOR}, {"^=", OpCode::BXOR},
            };
            auto it = COMPOUND.find(e.op);
            if (it == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
            emit(it->second, tmp, cur, val);
            free_temp(cur); free_temp(val);
            emit_write_local(loc.index, tmp);
            free_temp(tmp);
        } else {
            emit_write_local(loc.index, val);
            free_temp(val);
        }
    } else if (loc.kind == VarKind::CAPTURE) {
        // LOOK captures by value — a capture cannot be modified (philosophy)
        throw LookCompileError("Captured variable cannot be reassigned: $" + e.name);
    } else if (parent_ != nullptr || (scope_depth_ > 1 && !outer_globals_.count(e.name))) {   // 1 = the top level's own scope (compile_stmts)
        // Block scope AT TOP LEVEL too. A name assigned for the FIRST time in an if/for/while/try body
        // is that block's local (it already worked this way inside functions and in the interpreter; the VM at top level
        // made it a global → `if (c) { $z = 1 } print($z)` gave 1 in the VM and an error in the interpreter).
        // A name assigned at top level before the block (outer_globals_) stays global: the block writes to it.
        // Inside a function + the name is not a local/capture → create a new FUNCTION-LOCAL.
        // Scope isolation: a function cannot implicitly overwrite an outer (global) variable
        // (in the VM it used to leak via STORE_GLOBAL — since route
        // body locals were globals it also raced under concurrent requests).
        // Reading still falls to the global; since val (RHS) was compiled above,
        // a same-name read correctly saw the global.
        // (like Python's UnboundLocalError): if in this function the name was first read from OUTSIDE (or
        // `+=`/`.=` reads the outer value), creating a new local is a silent trap — `$n += 1` / `$n = $n + 1`
        // looks like it increments the outer counter, but the outer one never changes. It is an error even if the read is in another branch or
        // on an earlier line of the loop (reads from outside on the first iteration). `$n = 5` with no read stays local.
        if (parent_) {
            if (e.op != "=" || outer_reads_.count(e.name)) throw_outer_read_assign(e.name);
            local_made_at_.emplace(e.name, cur_line_);   // reverse order: an error if it is read from outside later, too
        }
        uint16_t result = val;
        if (e.op != "=") {
            // compound: read the current value from the GLOBAL (the name is not a local yet)
            uint32_t ni = add_const(Value(e.name));
            uint16_t cur = alloc_temp();
            emit(OpCode::LOAD_GLOBAL, cur, hi16(ni), lo16(ni));
            uint16_t tmp = alloc_temp();
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD}, {"-=", OpCode::SUB}, {"*=", OpCode::MUL},
                {"/=", OpCode::DIV}, {"%=", OpCode::MOD}, {".=", OpCode::CONCAT},
                {"&=", OpCode::BAND}, {"|=", OpCode::BOR}, {"^=", OpCode::BXOR},
            };
            auto cit = COMPOUND.find(e.op);
            if (cit == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
            emit(cit->second, tmp, cur, val);
            free_temp(cur); free_temp(val);
            result = tmp;
        }
        uint16_t slot = declare_local(e.name, 0);
        emit(OpCode::MOVE, slot, result);
        free_temp(result);
    } else {
        // Top-level (script global) → STORE_GLOBAL. Route/setup globals, app::
        // services and module references live here; the behavior is preserved.
        // The name is written to outer_globals_: if a later block assigns to this name it does not
        // create a new local, it writes to this global.
        outer_globals_.insert(e.name);   // only top level (outside a block) or a known global reaches this branch
        uint32_t ni = add_const(Value(e.name));
        // `.=` on a global is IN PLACE (APPEND_GLOBAL). OLD: read → new text → write; every step
        // copied the whole text (1 million `.=` in a top-level loop: 66 s).
        if (e.op == ".=") {
            emit(OpCode::APPEND_GLOBAL, val, hi16(ni), lo16(ni));
            free_temp(val);
            return;
        }
        // BUG FIX: this branch IGNORED e.op → top-level "$t += $i" was compiled as just
        // "$t = $i" (the left operand was dropped). The other three branches
        // (index, local, in-function) did compound correctly; only the GLOBAL
        // path was broken → the interpreter was RIGHT, the VM SILENTLY WRONG (engine divergence).
        // E.g. top-level: $t=0; $t+=5 → 5 in the VM (looks right) but
        // $t=1; $t+=2 → 2 (not 3); $s="x"; $s.="y" → "y" (not "xy").
        if (e.op != "=") {
            uint16_t cur = alloc_temp();
            emit(OpCode::LOAD_GLOBAL, cur, hi16(ni), lo16(ni));
            uint16_t tmp = alloc_temp();
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD}, {"-=", OpCode::SUB}, {"*=", OpCode::MUL},
                {"/=", OpCode::DIV}, {"%=", OpCode::MOD}, {".=", OpCode::CONCAT},
                {"&=", OpCode::BAND}, {"|=", OpCode::BOR}, {"^=", OpCode::BXOR},
            };
            auto cit = COMPOUND.find(e.op);
            if (cit == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
            emit(cit->second, tmp, cur, val);
            free_temp(cur); free_temp(val);
            val = tmp;
        }
        emit(OpCode::STORE_GLOBAL, val, hi16(ni), lo16(ni));
        free_temp(val);
    }
}

// ── compile_expr — dispatch ───────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_expr(const Expression& expr, uint16_t dest) {
    if (look_native_stack_low()) compile_stack_exhausted(expr.loc.line > 0 ? expr.loc.line : cur_line_);
    auto ensure_dest = [&]() -> uint16_t {
        return (dest == NO_REG) ? alloc_temp() : dest;
    };

    if (auto* e = node_as<NumberLiteral>(&expr)) {
        uint16_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (auto* e = node_as<FloatLiteral>(&expr)) {
        uint16_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (auto* e = node_as<BooleanLiteral>(&expr)) {
        uint16_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (node_as<NullLiteral>(&expr)) {
        uint16_t r = ensure_dest();
        emit(OpCode::LOAD_NULL, r);
        return r;
    }
    if (auto* e = node_as<StringLiteral>(&expr)) {
        uint16_t r = ensure_dest();
        // Does it contain interpolation? The lexer sends a raw string
        // Simple string (no interpolation) → LOAD_CONST
        // Interpolation → compile_string_interp
        if (!e->raw && e->value.find("{$") != std::string::npos) {
            return compile_string_interp(e->value, expr.loc.line, r);
        }
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }

    if (auto* e = node_as<Variable>(&expr)) {
        auto loc = resolve_var(e->name);
        if (loc.kind == VarKind::LOCAL) {
            // If no destination is given, the local's own register is returned (no copy).
            if (dest == NO_REG) return loc.index;
            uint16_t r = ensure_dest();
            emit_read_local(r, loc.index);
            return r;
        }
        uint16_t r = ensure_dest();
        if (loc.kind == VarKind::CAPTURE) {
            emit_read_capture(r, loc.index);
        } else {
            uint32_t ni = add_const(Value(e->name));
            if (parent_ && !e->name.empty() && e->name[0] == '$') {
                // The same name had been assigned as a local in this function (its block ended) — `if (c) { $n = 5 } else { print($n) }`
                auto made = local_made_at_.find(e->name);
                if (made != local_made_at_.end()) {
                    const std::string bare = e->name.substr(1);
                    const std::string who = proto_.name == "<closure>" ? "closure" : "function " + proto_.name + "()";
                    const std::string at = cur_line_ > 0 ? "line " + std::to_string(cur_line_) + ": " : "";
                    throw LookProgramError(at + who + " reads $" + bare + ", which it assigned only inside a block "
                        "that has ended (line " + std::to_string(made->second) + "); here $" + bare + " is the outer "
                        "variable, not that local — declare $" + bare + " before the block, or pass the value as a "
                        "parameter and return the new value", cur_line_);
                }
                outer_reads_.insert(e->name);
            }
            emit(OpCode::LOAD_GLOBAL, r, hi16(ni), lo16(ni));
            // A bare (no '$') name is read as a VALUE. OLD BUG: when LOAD_GLOBAL missed a bare name
            // it silently gave null (needed for "mod::fn"/call targets),
            // so a forgotten '$' — `if (is_admin)` — entered the WRONG BRANCH in the VM without an error;
            // tree-walk (the reference) threw "Undefined variable: is_admin".
            // Except for a call target (CALL names it), every bare read must be defined.
            if (!callee_ctx_ && !e->name.empty() && e->name[0] != '$'
                && e->name.find("::") == std::string::npos)
                emit(OpCode::CHECK_DEFINED, r, hi16(ni), lo16(ni));
        }
        return r;
    }

    if (auto* e = node_as<AssignmentExpression>(&expr)) {
        if (e->index) {
            // Element/field assignment: the value is the written element (the old value for postfix ++/--).
            uint16_t r = ensure_dest();
            assign_result_reg_ = r;
            compile_assign_expr(*e);
            return r;
        }
        compile_assign_expr(*e);
        // The assignment is used as an expression — read the value
        auto loc = resolve_var(e->name);
        uint16_t r = ensure_dest();
        if (loc.kind == VarKind::LOCAL) emit_read_local(r, loc.index);
        else {
            uint32_t ni = add_const(Value(e->name));
            emit(OpCode::LOAD_GLOBAL, r, hi16(ni), lo16(ni));
        }
        return r;
    }

    if (auto* e = node_as<BinaryExpression>(&expr)) {
        return compile_binary(*e, dest);
    }

    if (auto* e = node_as<UnaryExpression>(&expr)) {
        // ++/-- — it used to be unsupported in the VM (a compile error → an invisible
        // fall to the interpreter). Semantics: prefix returns the new value,
        // postfix returns the old value; the variable is written back.
        if (e->op == "++" || e->op == "--") {
            auto* var = node_as<Variable>(e->right.get());
            if (!var) throw LookCompileError("++/-- requires a variable");
            OpCode delta_op = (e->op == "++") ? OpCode::ADD : OpCode::SUB;
            uint16_t one = alloc_temp();
            emit_load_const(one, Value((int64_t)1), expr.loc.line);
            auto loc = resolve_var(var->name);
            if (loc.kind == VarKind::CAPTURE)
                throw LookCompileError("Captured variable cannot be reassigned: $" + var->name);
            if (loc.kind == VarKind::GLOBAL) check_no_outer_write(var->name, "it is changed with " + e->op);

            uint16_t r = ensure_dest();
            if (loc.kind == VarKind::LOCAL) {
                emit_read_local(r, loc.index);              // old value (for postfix)
                uint16_t nv = alloc_temp();
                emit(delta_op, nv, r, one);
                emit_write_local(loc.index, nv);
                if (e->prefix) emit(OpCode::MOVE, r, nv);   // prefix → new value
                free_temp(nv);
            } else {
                // GLOBAL
                uint32_t ni = add_const(Value(var->name));
                emit(OpCode::LOAD_GLOBAL, r, hi16(ni), lo16(ni));
                uint16_t nv = alloc_temp();
                emit(delta_op, nv, r, one);
                emit(OpCode::STORE_GLOBAL, nv, hi16(ni), lo16(ni));
                if (e->prefix) emit(OpCode::MOVE, r, nv);
                free_temp(nv);
            }
            free_temp(one);
            return r;
        }
        uint16_t operand = compile_expr(*e->right);
        uint16_t r = ensure_dest();
        if (e->op == "-")  emit(OpCode::UNM, r, operand);
        else if (e->op == "!") emit(OpCode::NOT, r, operand);
        else if (e->op == "~") emit(OpCode::BNOT, r, operand);
        else throw LookCompileError("Unknown unary op: " + e->op);
        free_temp(operand);
        return r;
    }

    if (auto* e = node_as<TernaryExpression>(&expr)) {
        uint16_t cond = compile_expr(*e->condition);
        int jf = emit_jump(OpCode::JUMP_IF_FALSE, cond);
        free_temp(cond);
        uint16_t r = ensure_dest();
        uint16_t then_r = compile_expr(*e->then_expr, r);
        if (then_r != r) { emit(OpCode::MOVE, r, then_r); free_temp(then_r); }
        int jend = emit_jump(OpCode::JUMP);
        patch_jump(jf, current_ip());
        uint16_t else_r = compile_expr(*e->else_expr, r);
        if (else_r != r) { emit(OpCode::MOVE, r, else_r); free_temp(else_r); }
        patch_jump(jend, current_ip());
        return r;
    }

    if (auto* e = node_as<CallExpression>(&expr)) {
        return compile_call(*e, dest);
    }

    if (auto* e = node_as<FunctionExpression>(&expr)) {
        return compile_closure(*e, dest);
    }

    if (auto* e = node_as<ArrayLiteral>(&expr)) {
        return compile_array_lit(*e, dest);
    }

    if (auto* e = node_as<AssocArrayLiteral>(&expr)) {
        return compile_assoc_lit(*e, dest);
    }

    if (auto* e = node_as<StructLiteralExpression>(&expr)) {
        return compile_struct_lit(*e, dest);
    }

    if (auto* e = node_as<IndexExpression>(&expr)) {
        uint16_t obj = compile_expr(*e->object);
        uint16_t idx = compile_expr(*e->index);
        uint16_t r   = ensure_dest();
        emit(OpCode::ARRAY_GET, r, obj, idx);
        free_temp(idx); free_temp(obj);
        return r;
    }

    if (auto* e = node_as<MemberAccessExpression>(&expr)) {
        uint16_t obj = compile_expr(*e->object);
        uint16_t r   = ensure_dest();
        // The field name goes into the constant pool — the field index is resolved at runtime
        uint32_t fi = add_const(Value(e->field));
        if (fi <= REG_LIMIT) {
            emit(OpCode::GET_FIELD, r, obj, u16(fi, "field name index"));
        } else {
            // Proto with 256+ constants: GET_FIELD's constant operand was 8 bits — it used to be
            // truncated by (fi & 0xFF) → the field was read with ANOTHER constant's name
            // (silently wrong data / null). CALL_BUILTIN got past the 256 wall with the NOP hint,
            // GET_FIELD/SET_FIELD were missed (symmetric surface). Load the field name
            // wide + a general ARRAY_GET — VM::get_field ≡ array_get, same semantics.
            uint16_t k = alloc_temp();
            emit_load_const(k, Value(e->field), 0);
            emit(OpCode::ARRAY_GET, r, obj, k);
            free_temp(k);
        }
        free_temp(obj);
        return r;
    }

    if (auto* e = node_as<ScopeResolution>(&expr)) {
        // module::func → LOAD_GLOBAL "module::func"
        std::string full = e->module_name + "::" + e->member_name;
        uint16_t r = ensure_dest();
        uint32_t ni = add_const(Value(full));
        emit(OpCode::LOAD_GLOBAL, r, hi16(ni), lo16(ni));
        return r;
    }

    throw LookCompileError("Unknown expression type: " + std::string(typeid(expr).name()),
                           expr.loc.line);
}

// ── compile_binary ────────────────────────────────────────────────────────────

// Opcodes of the binary operators (&&, ||, ?? are compiled separately).
static const std::unordered_map<std::string, OpCode>& binary_ops() {
    static const std::unordered_map<std::string, OpCode> OPS = {
        {"+",   OpCode::ADD},   {"-",  OpCode::SUB},  {"*",  OpCode::MUL},
        {"/",   OpCode::DIV},   {"%",  OpCode::MOD},  {"**", OpCode::POW},
        {".",   OpCode::CONCAT},{"..", OpCode::CONCAT},
        {"==",  OpCode::EQ},    {"!=", OpCode::NEQ},
        {"<",   OpCode::LT},    {">",  OpCode::GT},
        {"<=",  OpCode::LTE},   {">=", OpCode::GTE},
        {"<=>", OpCode::CMP3},
        {"&",   OpCode::BAND},  {"|",  OpCode::BOR},  {"^",  OpCode::BXOR},
        {"<<",  OpCode::SHL},   {">>", OpCode::SHR},
    };
    return OPS;
}

uint16_t FunctionCompiler::compile_binary(const BinaryExpression& e, uint16_t dest) {
    // Key point 2: && and || short-circuit
    if (e.op == "&&" || e.op == "||") return compile_logical(e, dest);

    // Null coalescing: a ?? b → a if a is not null, else b
    if (e.op == "??") {
        uint16_t lhs = compile_expr(*e.left);
        uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
        // Jump to rhs-block if lhs IS null
        int jnull = emit_jump(OpCode::JUMP_IF_NULL, lhs);
        // lhs is NOT null → use lhs
        emit(OpCode::MOVE, r, lhs);
        free_temp(lhs);
        int jend = emit_jump(OpCode::JUMP);
        // lhs IS null → evaluate rhs
        patch_jump(jnull, current_ip());
        uint16_t rhs = compile_expr(*e.right, r);
        if (rhs != r) { emit(OpCode::MOVE, r, rhs); free_temp(rhs); }
        patch_jump(jend, current_ip());
        return r;
    }

    // Left spine with a LOOP: `a + b + c + …` is a tree nested to the left like ((a+b)+c)+…; recursing over each node
    // meant 1000 C++ frames for a 1000-operator chain (measured: segfault on a 512 KB stack,
    // compile_binary × 580). The order is the same: the leftmost term, then from innermost to outermost the right
    // side and operation of each node. The spine ends at the first node that is not a binary operator with an opcode (&&, ||, ?? …).
    const auto& OPS = binary_ops();
    if (!OPS.count(e.op)) throw LookCompileError("Unknown binary op: " + e.op, e.loc.line);
    std::vector<const BinaryExpression*> spine;   // outermost to innermost
    const Expression* leftmost = &e;
    while (auto* b = node_as<BinaryExpression>(leftmost)) {
        if (!OPS.count(b->op)) break;
        spine.push_back(b);
        leftmost = b->left.get();
    }
    uint16_t l = compile_expr(*leftmost);
    for (size_t k = spine.size(); k-- > 0;) {
        const BinaryExpression& n = *spine[k];
        uint16_t r2 = compile_expr(*n.right);
        uint16_t r  = (k == 0 && dest != NO_REG) ? dest : alloc_temp();
        emit(OPS.at(n.op), r, l, r2);
        free_temp(r2); free_temp(l);
        l = r;
    }
    return l;
}

// ── compile_logical — short-circuit && / || ───────────────────────────────────

uint16_t FunctionCompiler::compile_logical(const BinaryExpression& e, uint16_t dest) {
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;

    // `a && b && c || d …` nested to the left: the spine is walked with a LOOP (see compile_binary). Each node, with its left's
    // result in r, writes its own short-circuit jump, then compiles its right into r; the jump
    // is tied to just after its right — the same instructions the recursive version produced.
    std::vector<const BinaryExpression*> spine;   // outermost to innermost
    const Expression* leftmost = &e;
    while (auto* b = node_as<BinaryExpression>(leftmost)) {
        if (b->op != "&&" && b->op != "||") break;
        spine.push_back(b);
        leftmost = b->left.get();
    }
    uint16_t lhs = compile_expr(*leftmost, r);
    if (lhs != r) { emit(OpCode::MOVE, r, lhs); free_temp(lhs); }

    for (size_t k = spine.size(); k-- > 0;) {
        const BinaryExpression& n = *spine[k];
        // &&: left false → result false, right is not evaluated; ||: left true → result true
        int short_circuit = emit_jump(n.op == "&&" ? OpCode::JUMP_IF_FALSE : OpCode::JUMP_IF_TRUE, r);
        uint16_t rhs = compile_expr(*n.right, r);
        if (rhs != r) { emit(OpCode::MOVE, r, rhs); free_temp(rhs); }
        patch_jump(short_circuit, current_ip());
    }
    return r;
}

// ── compile_call ──────────────────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_call(const CallExpression& e, uint16_t dest) {
    // Special functions: parallel, send, receive
    if (auto* sr = node_as<ScopeResolution>(e.callee.get())) {
        std::string full = sr->module_name + "::" + sr->member_name;
        int bidx = builtin_index(full);
        if (bidx >= 0) {
            // Known module function → CALL_BUILTIN. (timer::/jobs::worker/jobs::run are also wired to the VM on the
            // command line; ws::/sse:: give a clear error there — they used to drop the CLI
            // to tree-walk.)
            // Take a consecutive register block with alloc_seq — satisfies the VM's base+k assumption
            check_argc(e);
            uint16_t argc = u16(e.arguments.size(), "argument count");
            uint16_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
            for (int k = 0; k < argc; ++k) {
                uint16_t ev = compile_expr(*e.arguments[k], u16(base + k, "register index"));
                if (ev != u16(base + k, "register index")) emit(OpCode::MOVE, u16(base + k, "register index"), ev);
            }
            uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
            check_builtin_index(bidx, full);
            emit(OpCode::CALL_BUILTIN, r, lo16(bidx), base);
            // NOP hint: a=argc, b=the HIGH bits of the builtin index (16-bit index).
            // Bit-compatible: for idx<=255, b=0 = the old encoding. This is how the 256 wall was passed.
            emit(OpCode::NOP, argc, hi16(bidx));
            regs_->release_seq(base, argc);
            return r;
        }
        // Unknown module function → the general CALL path.
        // MARK: this call compiles but throws "Not callable" at RUNTIME
        // (LOAD_GLOBAL "mod::fn" → null → CALL). The command line / REPL / lk test see the flag
        // and error out without running (see CompiledProgram::uses_non_builtin_module_fn).
        mark_non_builtin_module_fn(full);
    }
    if (auto* var = node_as<Variable>(e.callee.get())) {
        // push($target, v) / pop($target) modify the VARIABLE that is the first argument. If the target is a
        // path (root is a variable: $a, $m["rows"], $u.tags), PUSH_PATH/POP_PATH do copy-on-write in the
        // root's own slot. The old builtin modified the shared vector in place:
        // `$b = $a; push($a, 1)` grew $b too.
        if ((var->name == "push" && e.arguments.size() == 2) || (var->name == "pop" && e.arguments.size() == 1)) {
            std::vector<std::pair<const Expression*, std::string>> rev;
            const Variable* root = nullptr;
            for (const Expression* cur = e.arguments[0].get(); cur; ) {
                if (auto* ix = node_as<IndexExpression>(cur)) {
                    rev.push_back({ix->index.get(), ""}); cur = ix->object.get();
                } else if (auto* ma = node_as<MemberAccessExpression>(cur)) {
                    rev.push_back({nullptr, ma->field}); cur = ma->object.get();
                } else { root = node_as<Variable>(cur); break; }
            }
            if (root && rev.size() <= 250) {
                const bool is_push = var->name == "push";
                auto rloc = resolve_var(root->name, /*for_write=*/false);
                if (rloc.kind == VarKind::GLOBAL) check_no_outer_write(root->name, var->name + "() changes it");
                const uint16_t n = u16(rev.size(), "index path length");
                uint16_t base = (n > 0) ? regs_->alloc_seq(n) : 0;
                for (uint16_t k = 0; k < n; ++k) {
                    const auto& item = rev[rev.size() - 1 - k];
                    const uint16_t kr = u16(base + k, "register index");
                    if (item.first) {
                        uint16_t ev = compile_expr(*item.first, kr);
                        if (ev != kr) emit(OpCode::MOVE, kr, ev);
                    } else {
                        emit_load_const(kr, Value(item.second), cur_line_);
                    }
                }
                uint16_t val = is_push ? compile_expr(*e.arguments[1]) : 0;
                uint16_t kind; uint32_t ix;
                if (rloc.kind == VarKind::LOCAL) {
                    kind = 0; ix = rloc.index;
                } else if (rloc.kind == VarKind::CAPTURE) {
                    throw LookCompileError("Captured variable cannot be changed: $" + root->name);
                } else {
                    kind = 4; ix = add_const(Value(root->name));
                }
                uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
                emit(is_push ? OpCode::PUSH_PATH : OpCode::POP_PATH, r, base, n);
                emit(OpCode::NOP, kind, hi16(ix), lo16(ix));
                if (is_push) { emit(OpCode::NOP, val); free_temp(val); }
                if (n > 0) regs_->release_seq(base, n);
                return r;
            }
        }
        if (var->name == "parallel") {
            if (e.arguments.size() != 1)
                throw LookCompileError("parallel() takes one argument", e.loc.line);
            uint16_t closure = compile_expr(*e.arguments[0]);
            emit(OpCode::PARALLEL_CALL, closure);
            free_temp(closure);
            uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "send") {
            if (e.arguments.size() != 2)
                throw LookCompileError("send() takes 2 arguments", e.loc.line);
            uint16_t ch  = compile_expr(*e.arguments[0]);
            uint16_t val = compile_expr(*e.arguments[1]);
            emit(OpCode::CHAN_SEND, ch, val);
            free_temp(val); free_temp(ch);
            uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "receive") {
            if (e.arguments.size() != 1)
                throw LookCompileError("receive() takes 1 argument", e.loc.line);
            uint16_t ch = compile_expr(*e.arguments[0]);
            uint16_t r  = (dest == NO_REG) ? alloc_temp() : dest;
            emit(OpCode::CHAN_RECV, r, ch);
            free_temp(ch);
            return r;
        }
        if (var->name == "close") {
            // There was NO argument-count check → calling close() with no arguments dereferenced null on
            // the empty e.arguments vector → COMPILE-TIME SEGFAULT (tree-walk gave a graceful error
            // → divergence). Validate with the send/receive pattern.
            if (e.arguments.size() != 1)
                throw LookCompileError("close() takes 1 argument", e.loc.line);
            uint16_t ch = compile_expr(*e.arguments[0]);
            emit(OpCode::CHAN_CLOSE, ch);
            free_temp(ch);
            uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "chan_size") {
            if (e.arguments.size() != 1)
                throw LookCompileError("chan_size() takes 1 argument", e.loc.line);
            uint16_t ch = compile_expr(*e.arguments[0]);
            uint16_t r  = (dest == NO_REG) ? alloc_temp() : dest;
            emit(OpCode::CHAN_SIZE, r, ch);
            free_temp(ch);
            return r;
        }
        // Global builtin aliases → the VM builtin equivalent.
        // These names were resolved inline in the old interpreter but were not in the VM builtin list
        // → when a route called them they compiled to CALL (bytecode-fn), threw
        // "Not callable" at runtime, and the route fell PERMANENTLY to the interpreter
        // (all VM optimizations, including O(n²) string handling, were wasted on that route).
        static const std::unordered_map<std::string, std::string> BUILTIN_ALIAS = {
            {"len", "count"}, {"intval", "int"}, {"floatval", "float"},
            {"strval", "string"}, {"boolval", "bool"},
            {"json", "json::encode"},
            // Web: bare header()/redirect() were globals of the old interpreter and fell back in the VM.
            // The response::header/redirect module functions ALREADY exist and are bound to the
            // per-request web_ctx_ (like response::text) + they do CRLF
            // sanitizing (a bonus).
            {"header", "response::header"}, {"redirect", "response::redirect"},
        };
        std::string bname = var->name;
        if (auto ai = BUILTIN_ALIAS.find(bname); ai != BUILTIN_ALIAS.end())
            bname = ai->second;
        // A known built-in?
        int bidx = builtin_index(bname);
        if (bidx >= 0) {
            // Take a consecutive register block with alloc_seq — satisfies the VM's base+k assumption
            check_argc(e);
            uint16_t argc = u16(e.arguments.size(), "argument count");
            uint16_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
            for (int k = 0; k < argc; ++k) {
                uint16_t ev = compile_expr(*e.arguments[k], u16(base + k, "register index"));
                if (ev != u16(base + k, "register index")) emit(OpCode::MOVE, u16(base + k, "register index"), ev);
            }
            uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
            check_builtin_index(bidx, bname);
            emit(OpCode::CALL_BUILTIN, r, lo16(bidx), base);
            // NOP hint: a=argc, b=the HIGH bits of the builtin index (16-bit index).
            // Bit-compatible: for idx<=255, b=0 = the old encoding. This is how the 256 wall was passed.
            emit(OpCode::NOP, argc, hi16(bidx));
            regs_->release_seq(base, argc);
            return r;
        }
    }

    // General CALL: put the callee in a register, lay out the arguments in order
    // callee_ctx_ is turned on only when the callee is directly a NAME (so it does not leak into nested expressions).
    callee_ctx_ = node_as<Variable>(e.callee.get()) != nullptr;
    uint16_t fn = compile_expr(*e.callee);
    callee_ctx_ = false;
    // Take a consecutive register block with alloc_seq — satisfies the VM's base+k assumption
    check_argc(e);
    uint16_t argc = u16(e.arguments.size(), "argument count");
    uint16_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
    for (int k = 0; k < argc; ++k) {
        uint16_t ev = compile_expr(*e.arguments[k], u16(base + k, "register index"));
        if (ev != u16(base + k, "register index")) emit(OpCode::MOVE, u16(base + k, "register index"), ev);
    }
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
    emit(OpCode::CALL, r, fn, base);
    // argc is embedded in the NOP as an extra byte — the VM reads it.
    // If the callee is a simple NAME, the name's constant index (ni+1, 32-bit) is also embedded
    // in the NOP's b/c fields — so the VM CAN SAY the name in its error message.
    // OLD BUG: at the VM's CALL site there was only the register value, no name:
    //   nonexistent_function(1)
    //   VM        -> "Not callable (BYTECODE_FN expected)"   (which name?)
    //   tree-walk -> "Undefined variable: nonexistent_function"
    // LOAD_GLOBAL cannot throw on this name because "mod::fn" names MUST miss there
    // (so they fall to the general CALL path) — hence the fix is to carry the name here.
    // 0 = no name (the same method as CALL_BUILTIN's 16-bit index pattern).
    uint32_t cname = 0;
    if (auto* cv = node_as<Variable>(e.callee.get()))
        cname = u32((long long)add_const(Value(cv->name)) + 1, "constant index");
    emit(OpCode::NOP, argc, hi16(cname), lo16(cname));
    regs_->release_seq(base, argc);
    free_temp(fn);
    return r;
}

// ── compile_closure ───────────────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_closure(const FunctionExpression& e, uint16_t dest) {
    // Resolve the captures in the use() list
    // Each capture: take the value from the parent → snapshot into Closure.captures[]
    FunctionCompiler inner("<closure>", e.parameters, e.is_variadic, this);

    // Tell inner the use() list (capture indices 0..k-1).
    if ((long long)e.captures.size() > REG_LIMIT)
        throw LookCompileError("closure captures more than 65534 variables", e.loc.line);
    for (size_t i = 0; i < e.captures.size(); ++i)
        inner.captures_.push_back({e.captures[i], u16(i, "capture count")});

    // fn($x) => expression: a single return with no body line; take the line from the closure itself
    // (otherwise its instructions had no line — errors and the call chain showed no line).
    inner.cur_line_ = e.loc.line > 0 ? e.loc.line : cur_line_;
    auto proto = inner.compile(*e.body, &e.defaults);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;

    // Load the capture values BEFORE MAKE_CLOSURE — right after MAKE_CLOSURE the VM
    // expects consecutive LOAD_CAPTURE(0, cr) hints; a MOVE cannot come in between.
    // inner.captures_ = explicit use() (0..k-1) + the automatic captures discovered
    // while compiling the body (k..). Load all of them from the parent scope in index order.
    std::vector<uint16_t> cap_regs;
    for (auto& cap : inner.captures_) {
        const std::string& cap_name = cap.name;
        auto loc = resolve_var(cap_name);
        uint16_t cr = alloc_temp();
        if      (loc.kind == VarKind::LOCAL)   emit(OpCode::MOVE, cr, loc.index);
        else if (loc.kind == VarKind::CAPTURE) emit(OpCode::LOAD_CAPTURE, cr, loc.index);
        else {
            uint32_t ni = add_const(Value(cap_name));
            emit(OpCode::LOAD_GLOBAL, cr, hi16(ni), lo16(ni));
        }
        cap_regs.push_back(cr);
    }

    emit_make_closure(r, fn_idx);

    // Consecutive hints right after MAKE_CLOSURE — the VM reads this pattern
    for (uint16_t cr : cap_regs) {
        emit(OpCode::LOAD_CAPTURE, 0, cr);
        free_temp(cr);
    }
    return r;
}

// ── compile_string_interp ─────────────────────────────────────────────────────
//
// "{$name} hello {$x+$y}" → CONCAT chain
// TODO: a full expression parser — for now {$var} and constant parts are supported

uint16_t FunctionCompiler::compile_string_interp(const std::string& raw, int line, uint16_t dest) {
    // A string with no interpolation → load directly
    if (raw.find('{') == std::string::npos) {
        uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
        emit_load_const(r, Value(raw), line);
        return r;
    }

    // Split into parts: constant string + interpolation, alternating. Respect nested braces;
    // the fragment is lexed+parsed+compiled as a FULL expression ({$a + $b}, {$arr[1]},
    // {count($x)}, {mod::f()}...); on a parse error {…} stays literal.
    // Otherwise the VM would silently produce "null".
    // The trigger is ONLY '{$' — '$'-prefixed forms are definitely interpolation. '{identifier}'
    // without a dollar is DELIBERATELY left out: the old interpreter turned it into a literal on an eval error,
    // while the VM would compile it and produce null → this would change web behavior for template strings
    // like '{renk}'. A safe/consistent boundary: '{$...}'.
    auto is_interp_start = [](const std::string& s, size_t p) -> bool {
        return s[p] == '{' && p + 1 < s.size() && s[p + 1] == '$';
    };
    std::vector<uint16_t> parts;
    std::vector<std::unique_ptr<Program>> keepalive; // parsed ASTs must live until compile finishes
    size_t i = 0;
    while (i < raw.size()) {
        if (is_interp_start(raw, i)) {
            // Matching '}' — respect nested braces
            size_t depth = 1, j = i + 1;
            while (j < raw.size() && depth > 0) {
                if (raw[j] == '{') depth++;
                else if (raw[j] == '}') { if (--depth == 0) break; }
                ++j;
            }
            if (depth != 0) {  // unclosed brace → literal '{', move on
                uint16_t pr = alloc_temp();
                emit_load_const(pr, Value(std::string(1, raw[i])), line);
                parts.push_back(pr);
                ++i;
                continue;
            }
            std::string expr_src = raw.substr(i + 1, j - i - 1);
            const std::string literal_src = raw.substr(i, j - i + 1);  // "{$x}" — for the error case
            uint16_t pr = NO_REG;
            try {
                Lexer lex(expr_src + ";");
                Parser par(lex.scan_tokens());
                auto prog = par.parse();
                if (!prog->statements.empty()) {
                    if (auto* es = node_as<ExpressionStatement>(prog->statements[0].get())) {
                        // The fragment is wrapped in a RUNTIME try/catch: an eval error (e.g.
                        // an undefined variable → strict LOAD_GLOBAL throws) leaves "{…}" as a
                        // literal (the old interpreter's interpolate_string() rule).
                        // Parse errors are already handled at compile time below;
                        // this wrapper is for EVAL errors.
                        pr = alloc_temp();
                        int try_push_ip = emit(OpCode::TRY_PUSH, 0, 0, 0);
                        // Compile into a natural register (dest=255): the CALL result depends on the arg-register
                        // layout, forcing a fixed dest would break it. Then MOVE to the temp
                        // + TO_STR — if a local slot is returned we do not write over it.
                        uint16_t src = compile_expr(*es->expression);
                        emit(OpCode::MOVE, pr, src);
                        emit(OpCode::TO_STR, pr, pr);
                        emit(OpCode::TRY_POP);
                        int jump_end = emit_jump(OpCode::JUMP);
                        patch_jump(try_push_ip, current_ip());          // catch entry
                        emit_load_const(pr, Value(literal_src), line);  // error → literal
                        patch_jump(jump_end, current_ip());
                        keepalive.push_back(std::move(prog));
                    }
                }
            } catch (...) { pr = NO_REG; }
            if (pr == NO_REG) {  // parse error → {…} literal
                pr = alloc_temp();
                emit_load_const(pr, Value(literal_src), line);
            }
            parts.push_back(pr);
            i = j + 1;
        } else {
            // Literal — up to the next interpolation trigger
            std::string literal;
            while (i < raw.size() && !is_interp_start(raw, i)) literal += raw[i++];
            if (!literal.empty()) {
                uint16_t pr = alloc_temp();
                emit_load_const(pr, Value(literal), line);
                parts.push_back(pr);
            }
        }
    }

    if (parts.empty()) {
        uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
        emit(OpCode::LOAD_NULL, r);
        return r;
    }
    if (parts.size() == 1) {
        if (dest != NO_REG && parts[0] != dest) { emit(OpCode::MOVE, dest, parts[0]); free_temp(parts[0]); return dest; }
        return parts[0];
    }

    // CONCAT chain: r = parts[0] . parts[1] . ...
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
    emit(OpCode::CONCAT, r, parts[0], parts[1]);
    free_temp(parts[0]); free_temp(parts[1]);
    for (size_t j = 2; j < parts.size(); ++j) {
        emit(OpCode::CONCAT, r, r, parts[j]);
        free_temp(parts[j]);
    }
    return r;
}

// ── compile_array_lit ─────────────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_array_lit(const ArrayLiteral& e, uint16_t dest) {
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
    emit(OpCode::NEW_ARRAY, r, hint_u16(e.elements.size()));
    for (auto& el : e.elements) {
        uint16_t er = compile_expr(*el);
        emit(OpCode::ARRAY_PUSH, r, er);
        free_temp(er);
    }
    return r;
}

// ── compile_assoc_lit ─────────────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_assoc_lit(const AssocArrayLiteral& e, uint16_t dest) {
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
    // Pass the final element count (sentinel + 2 per pair) so the VM can reserve the
    // vector once instead of growing it through log(n) reallocations — each realloc also
    // re-copies the boxed key Values (atomic refcount traffic), so reserving is a real win.
    size_t need = 1 + 2 * e.pairs.size();
    emit(OpCode::NEW_ASSOC, r, hint_u16(need));
    // If every key is a distinct compile-time string literal, the per-insert dedup scan in
    // array_set is provably redundant (no key can collide), so emit the scan-free
    // ASSOC_APPEND — turning O(n^2) literal construction into O(n). Any dynamic key, numeric
    // key, or duplicate literal falls back to ARRAY_SET, which keeps the correct last-wins
    // dedup semantics. User-facing $a["k"]=v is unaffected (still ARRAY_SET).
    bool distinct_string_keys = true;
    std::unordered_set<std::string> seen;
    for (auto& [k, v] : e.pairs) {
        auto* sl = node_as<StringLiteral>(k.get());
        if (!sl || !seen.insert(sl->value).second) { distinct_string_keys = false; break; }
    }
    OpCode set_op = distinct_string_keys ? OpCode::ASSOC_APPEND : OpCode::ARRAY_SET;
    for (auto& [k, v] : e.pairs) {
        uint16_t kr = compile_expr(*k);
        uint16_t vr = compile_expr(*v);
        emit(set_op, r, kr, vr);
        free_temp(vr); free_temp(kr);
    }
    return r;
}

// ── compile_struct_lit ────────────────────────────────────────────────────────

uint16_t FunctionCompiler::compile_struct_lit(const StructLiteralExpression& e, uint16_t dest) {
    uint16_t r = (dest == NO_REG) ? alloc_temp() : dest;
    uint32_t ni = add_const(Value(e.struct_name));
    emit(OpCode::NEW_STRUCT, r, hi16(ni), lo16(ni));
    // Field names of the literal — for the VM's "Unknown field" validation.
    // NOP(n) + n×LOAD_CONST_W: 16-bit constants (not truncated); the VM's NEW_STRUCT
    // READS this block and SKIPS it, none of it is executed (does not write to register 0).
    if ((long long)e.fields.size() > REG_LIMIT)
        throw std::runtime_error("struct literal '" + e.struct_name + "' has more than 65534 fields");
    emit(OpCode::NOP, u16(e.fields.size(), "struct field count"));
    for (auto& [fname, fval] : e.fields) {
        uint32_t fi = add_const(Value(fname));
        emit(OpCode::LOAD_CONST_W, 0, hi16(fi), lo16(fi));
    }
    for (auto& [fname, fval] : e.fields) {
        uint32_t fi = add_const(Value(fname));
        uint16_t  vr = compile_expr(*fval);
        if (fi <= REG_LIMIT) {
            emit(OpCode::SET_FIELD, r, u16(fi, "field name index"), vr);
        } else {
            // 256+ constants: SET_FIELD's 8-bit operand would overflow → the field was
            // written with the WRONG name (like {"c46":11}). A fallback symmetric with GET_FIELD:
            // VM::set_field ≡ array_set.
            uint16_t k = alloc_temp();
            emit_load_const(k, Value(fname), 0);
            emit(OpCode::ARRAY_SET, r, k, vr);
            free_temp(k);
        }
        free_temp(vr);
    }
    return r;
}

// ── FunctionCompiler::compile_stmts — program-level entry ────────────────────

std::shared_ptr<FunctionProto> FunctionCompiler::compile_stmts(
    const std::vector<std::unique_ptr<Statement>>& stmts)
{
    push_scope();
    for (auto& s : stmts) compile_stmt(*s);
    pop_scope();
    emit(OpCode::RETURN_NULL);
    proto_.reg_count = regs_->max_used();
    return std::make_shared<FunctionProto>(std::move(proto_));
}

// ── Compiler::compile — public entry point ────────────────────────────────────

CompiledProgram Compiler::compile(const Program& program, const std::string& base_dir,
                                  const std::string& main_file) {
    g_compile_root = base_dir;
    g_compile_included.clear();
    g_compile_file.clear();
    g_compile_stack.clear();
    g_compile_stack_names.clear();
    if (!main_file.empty()) {   // the main script is on the stack too: a.lk → b.lk → a.lk is a cycle
        std::error_code ec;
        const std::string abs = cfs::weakly_canonical(cfs::absolute(main_file), ec).string();
        g_compile_stack.push_back(ec ? main_file : abs);
        g_compile_stack_names.push_back(cfs::path(main_file).filename().generic_string());
    }
    g_compile_structs.clear();
    g_compile_cur_dir.clear();
    g_use_modules.clear();
    g_compile_modules.clear();
    if (!base_dir.empty()) g_compile_cur_dir.push_back(base_dir);
    FunctionCompiler main_compiler("<main>", {}, false, nullptr);
    auto proto = main_compiler.compile_stmts(program.statements);

    CompiledProgram out;
    out.main_proto = proto;
    out.use_modules = std::move(g_use_modules);
    g_use_modules.clear();
    out.uses_non_builtin_module_fn = main_compiler.used_non_builtin_module_fn();
    out.non_builtin_module_fns     = main_compiler.non_builtin_module_fn_names();
    return out;
}

CompiledProgram Compiler::compile(const Program& program, const std::string& base_dir,
                                  const std::set<std::string>& host_modules) {
    g_host_modules = host_modules;
    struct Reset { ~Reset() { g_host_modules.clear(); } } reset;   // so the next compilation is not affected
    return compile(program, base_dir);
}

} // namespace look

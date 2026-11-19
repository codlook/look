#pragma once

#include "look/runtime_host.h"  // Value

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace look {

// ── OpCode ────────────────────────────────────────────────────────────────────
//
// 3-address register-based instruction set.
// Each instruction: opcode (1B) + a,b,c register/index fields (2B each)
// For a wide constant, OP_LOAD_CONST_W: a=reg, b<<16|c = 32-bit pool index.

enum class OpCode : uint8_t {
    // ── Constant loading ──────────────────────────────────────────────────────
    LOAD_CONST,      // r[a] = pool[b]          — b < 65536
    LOAD_CONST_W,    // r[a] = pool[b<<16|c]    — 32-bit pool index
    LOAD_NULL,       // r[a] = null
    LOAD_TRUE,       // r[a] = true
    LOAD_FALSE,      // r[a] = false
    LOAD_INT,        // r[a] = (int16_t)b       — small literal (-32768..32767)

    // ── Register / variable ───────────────────────────────────────────────────
    MOVE,            // r[a] = r[b]
    LOAD_VAR,        // r[a] = vars[b]          — locals/upvalues
    STORE_VAR,       // vars[a] = r[b]
    LOAD_GLOBAL,     // r[a] = globals[pool[b]] — string key
    STORE_GLOBAL,    // globals[pool[b]] = r[a]

    // ── Aritmetik (r[a] = r[b] OP r[c]) ──────────────────────────────────────
    ADD, SUB, MUL, DIV, MOD, POW,
    UNM,             // r[a] = -r[b]            — unary minus

    // ── Bitwise ───────────────────────────────────────────────────────────────
    BAND, BOR, BXOR, BNOT, SHL, SHR,

    // ── String ────────────────────────────────────────────────────────────────
    CONCAT,          // r[a] = r[b] . r[c]
    // String interpolation: the compiler splits it into parts and emits a CONCAT chain.

    // ── Comparison (r[a] = bool) ──────────────────────────────────────────────
    EQ, NEQ, LT, GT, LTE, GTE,
    CMP3,            // r[a] = r[b] <=> r[c]   — -1/0/1

    // ── Logical ───────────────────────────────────────────────────────────────
    NOT,             // r[a] = !r[b]
    // AND/OR short-circuit: built from JUMP_IF_FALSE / JUMP_IF_TRUE

    // ── Null coalescing ───────────────────────────────────────────────────────
    COALESCE,        // r[a] = r[b] ?? r[c]

    // ── Array ─────────────────────────────────────────────────────────────────
    NEW_ARRAY,       // r[a] = []               — b = hint capacity
    NEW_ASSOC,       // r[a] = {}
    ARRAY_GET,       // r[a] = r[b][r[c]]
    ARRAY_SET,       // r[a][r[b]] = r[c]
    ARRAY_PUSH,      // push(r[a], r[b])
    ARRAY_LEN,       // r[a] = count(r[b])

    // ── Struct ────────────────────────────────────────────────────────────────
    NEW_STRUCT,      // r[a] = StructDef[pool[b]]{}   — compile-time def index
    GET_FIELD,       // r[a] = r[b].field[c]          — c = field index (compile-time)
    SET_FIELD,       // r[a].field[b] = r[c]
    // The "field index" is computed at compile time from the order of struct_def.fields[].

    // ── Control flow ──────────────────────────────────────────────────────────
    JUMP,            // ip = a<<16|b             — 16-bit offset
    JUMP_IF_FALSE,   // if !r[a]: ip = b<<16|c
    JUMP_IF_TRUE,    // if  r[a]: ip = b<<16|c
    JUMP_IF_NULL,    // if r[a]==null: ip = b<<16|c

    // ── Function call ─────────────────────────────────────────────────────────
    // Call convention:
    //   CALL r_fn, r_args_base, argc
    //   Arguments must be ready in the range r_args_base .. r_args_base+argc-1.
    //   The result is written to r[a].
    CALL,            // r[a] = call r[b](r[b+1]..r[b+c])
    CALL_BUILTIN,    // r[a] = builtins[b](r[c]...) — c=args_base, count in next byte
    TAIL_CALL,       // tail call optimization — CALL without RETURN
    RETURN,          // return r[a]
    RETURN_NULL,     // return null

    // ── Closure ───────────────────────────────────────────────────────────────
    MAKE_CLOSURE,    // r[a] = Closure{fn_pool[b], captures...}
                     // Next N instructions: CAPTURE_VAL r[x] (extra compile-time info)
    LOAD_CAPTURE,    // r[a] = current_closure.captures[b]
    // No STORE_CAPTURE — LOOK captures by value, no mutation

    // ── Iteration ─────────────────────────────────────────────────────────────
    FOR_PREP,        // r[a]=iter_state, r[a+1]=0(idx)  — r[b]=array
    FOR_STEP,        // if end: ip=c<<16|d; else r[a+2]=val, r[a+3]=idx, ++r[a+1]
    // foreach ($arr as $v)        → r[a+2]=value
    // foreach ($arr as $k => $v)  → r[a+2]=key, r[a+3]=value

    // ── Error handling ────────────────────────────────────────────────────────
    TRY_PUSH,        // catch_stack.push(ip=a<<16|b)    — start of the catch block
    TRY_POP,         // catch_stack.pop()
    THROW,           // throw r[a]
    LOAD_EXC,        // r[a] = current_exception (at the start of the catch block)
    LOAD_ARGC,       // r[a] = number of args supplied at the call (default-param prologue)

    // ── LOOK-specific ─────────────────────────────────────────────────────────
    PARALLEL_CALL,   // spawn thread: closure=r[a]     — fire and forget
    CHAN_SEND,        // send(r[a], r[b])
    CHAN_RECV,        // r[a] = receive(r[b])
    CHAN_CLOSE,       // close(r[a])
    CHAN_SIZE,        // r[a] = chan_size(r[b])
    WS_SEND,         // ws::send(r[a], r[b])
    WS_BROADCAST,    // ws::broadcast(r[a])
    SSE_SEND,        // sse::send(r[a], r[b] [, r[c]])
    ROUTE_MATCH,     // dispatcher — internal, a single instruction

    // ── Type / cast ───────────────────────────────────────────────────────────
    TYPE_OF,         // r[a] = type::of(r[b])
    TO_INT,          // r[a] = int(r[b])
    TO_FLOAT,        // r[a] = float(r[b])
    TO_STR,          // r[a] = string(r[b])
    TO_BOOL,         // r[a] = bool(r[b])

    // ── Debug ─────────────────────────────────────────────────────────────────
    NOP,
    BREAKPOINT,      // VM debug hook

    // Appended at the END of the enum on purpose: inserting an opcode in the middle
    // renumbers every later opcode and perturbs the VM dispatch switch's codegen, which
    // measurably slowed the hot arithmetic/loop opcodes (~13% on int_arith/loop). New
    // opcodes go here to keep the hot opcodes' numbering — and jump-table slots — stable.
    ASSOC_APPEND,    // push key r[b] + val r[c] onto assoc r[a] WITHOUT the dedup scan —
                     // emitted only for assoc literals whose keys the compiler has proven
                     // are distinct string literals, so the runtime scan is provably redundant
    CHECK_DEFINED,   // throw "Undefined variable: <const b<<16|c>" when r[a] is null — emitted
                     // right after LOAD_GLOBAL for a bare (no '$') name read as a VALUE
    SET_PATH,        // copy-on-write assignment through an index path:
                     //   root[k0][k1]...[kn-1] = r[a];  keys in r[b]..r[b+c-1];  next NOP: a = root
                     //   kind (0 local, 4 global), b<<16|c = its
                     //   register / name constant. Each level is detached (copied if
                     //   shared) in the variable's own slot before the write.
    PUSH_PATH,       // r[a] = push(root[k0]..[kc-1], value): keys in r[b]..; next NOP = root (as SET_PATH),
                     //   then a NOP whose a = the value register. Appends to the VARIABLE (copy on write).
                     //   That NOP's b = 1: compiled from `$a = array::push($a, v)` (its error message).
    POP_PATH,        // r[a] = pop(root[k0]..[kc-1]); next NOP = root. Removes the last element.
    APPEND_GLOBAL,   // globals[pool[b<<16|c]] .= r[a] IN PLACE (`$s .= x`, `$s = $s . x` at top level;
                     //   copied once if shared). "Undefined variable" when the global does not exist.
    SHRINK_PATH,     // root = array::slice(root, 0, -1) IN PLACE (`$a = array::slice($a, 0, -1)`); next NOP =
                     //   root (as SET_PATH). Same errors as the call; an empty list stays empty.
};

// ── Instruction ───────────────────────────────────────────────────────────────

// Operands are 16 bits (they were 8 bits in 1.x → a limit of 255 registers / parameters /
// captures per function and 65535 constants; lifted so that "the app does not start if the VM
// cannot compile it" never trips). Two operands combine into a 32-bit field: b<<16|c (constant index, jump target).
struct Instruction {
    OpCode   op;
    uint8_t  pad_ = 0;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;

    // Helpers
    static Instruction make(OpCode op, uint16_t a=0, uint16_t b=0, uint16_t c=0) {
        return {op, 0, a, b, c};
    }
    // 32-bit b<<16|c field (constant index, jump target, name constant)
    uint32_t bx() const { return (uint32_t(b) << 16) | c; }
    // 32-bit b|c<<16 field — MAKE_CLOSURE's inner function index (b low, c high)
    uint32_t bx_lohi() const { return uint32_t(b) | (uint32_t(c) << 16); }
    // CALL_BUILTIN's builtin index: the low half is in this instruction's b, the high half
    // in the b of the NOP that follows. Operands are combined ONLY through these helpers —
    // tests/no_raw_operand_decode.sh forbids hand-written bit shifts (when the layout changes,
    // a missed reader silently reads a wrong value: it happened in the route scanner).
    static uint32_t builtin_index(const Instruction& call, const Instruction& nop_hint) {
        return uint32_t(call.b) | (uint32_t(nop_hint.b) << 16);
    }
};

static_assert(sizeof(Instruction) == 8, "Instruction must be 8 bytes");

// ── Constant pool ─────────────────────────────────────────────────────────────
//
// Per-function. Every constant literal (number, string, bool, null) goes here.
// A function's constant count is practically unlimited (LOAD_CONST_W is 32-bit).

using ConstantPool = std::vector<Value>;

// ── FunctionProto — compiler output ──────────────────────────────────────────
//
// Produced for every LOOK function (lambdas included).
// While the VM runs, CallFrame holds a pointer to this proto.

struct FunctionProto {
    std::string                          name;         // debug
    int                                  arity  = 0;   // number of required parameters
    int                                  required = 0; // number of params without a default (and not the variadic rest) — missing-argument check
    bool                                 variadic = false;
    int                                  reg_count = 0;// max register count (computed by the compiler)
    ConstantPool                         constants;
    std::vector<Instruction>             code;
    std::vector<std::shared_ptr<FunctionProto>> nested; // inner functions

    // Source location info — line number for each instruction
    std::vector<int>                     lines;
    // ...and the file: (first instruction, file) — one row only when the file changes; "" = main script.
    // The top-level code of an included file is compiled into the main function → one proto
    // may span several files. The file name is relative to the main script's directory (lib/util.lk).
    std::vector<std::pair<int, std::string>> file_runs;
    const std::string& file_at(int ip) const {
        static const std::string kMain;
        const std::string* f = &kMain;
        for (const auto& r : file_runs) { if (r.first > ip) break; f = &r.second; }
        return *f;
    }

    // Parameter names (for error messages)
    std::vector<std::string>             params;
};

// ── Closure object — runtime ──────────────────────────────────────────────────
//
// FunctionProto + captured values.
// A snapshot is taken when handed to a parallel() thread — it does not change afterwards.

struct Closure {
    std::shared_ptr<FunctionProto> proto;
    std::vector<Value>             captures; // in use ($a,$b,...) order
    // Host (C++) function — when set, a call runs this instead of bytecode.
    // For names not in the builtin table and provided only by a runner (`lk test`:
    // test(), assert_eq(), assert::eq …): bound as a global, called like a LOOK function,
    // and can be passed as a callback. proto stays empty-but-valid (variadic,
    // no registers) → every place that touches the proto (argument check, copy) works.
    std::function<Value(std::vector<Value>&)> native;

    explicit Closure(std::shared_ptr<FunctionProto> p) : proto(std::move(p)) {}
    // Closure chains (each captures the previous one) are released non-recursively, like ValueArray.
    ~Closure() { if (!captures.empty()) look_release_values(captures); }
    Closure(const Closure&) = default;
    Closure(Closure&&) = default;
    Closure& operator=(const Closure&) = default;
    Closure& operator=(Closure&&) = default;
};

// ── CallFrame — VM call stack ─────────────────────────────────────────────────

struct CallFrame {
    std::shared_ptr<Closure> closure;
    int                      ip      = 0;   // instruction pointer
    int                      base    = 0;   // start offset in the register array
    int                      ret_reg = 0;   // register where the caller wants the result
};

// ── CompiledProgram — bytecode output of the whole program ───────────────────

struct CompiledProgram {
    std::shared_ptr<FunctionProto> main_proto; // top-level
    // Does the program call a "mod::fn" that is NOT a builtin? (e.g. cache::keys —
    // present in the module table but not in builtin_names). Such a call compiles but
    // throws "not callable" at RUNTIME. The CLI / REPL / lk test see this flag and
    // report a "does not exist" error without running anything (BEFORE EXECUTION).
    bool uses_non_builtin_module_fn = false;
    std::vector<std::string> non_builtin_module_fns;   // names (LOOK_VM_STRICT report)
    // stdlib modules the program (including included files) `use`s, in the order seen.
    // The runner opens them AFTER compilation, then builds the builtin table. OLD WAY: a separate
    // scan (collect_use_modules) parsed every included file a SECOND time — qr-menu
    // was 37% of a CGI request because of it.
    std::vector<std::string> use_modules;
    // Struct definitions and route registrations are set up by running the setup phase.
};

} // namespace look

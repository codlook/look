#pragma once

#include "look/interpreter.h"  // Value

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace look {

// ── OpCode ────────────────────────────────────────────────────────────────────
//
// 3-adres register tabanlı instruction set.
// Her instruction: opcode (1B) + a,b,c register/index alanları (1B her biri)
// Geniş sabit için OP_LOAD_CONST_W: a=reg, bhi+blo=16-bit pool index.

enum class OpCode : uint8_t {
    // ── Sabit yükleme ─────────────────────────────────────────────────────────
    LOAD_CONST,      // r[a] = pool[b]          — b < 256
    LOAD_CONST_W,    // r[a] = pool[b<<8|c]     — 16-bit pool index
    LOAD_NULL,       // r[a] = null
    LOAD_TRUE,       // r[a] = true
    LOAD_FALSE,      // r[a] = false
    LOAD_INT,        // r[a] = (int8_t)b        — küçük literal (-128..127)

    // ── Register / değişken ───────────────────────────────────────────────────
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
    // String interpolation: compiler parçalara ayırıp CONCAT zinciri üretir.

    // ── Karşılaştırma (r[a] = bool) ───────────────────────────────────────────
    EQ, NEQ, LT, GT, LTE, GTE,
    CMP3,            // r[a] = r[b] <=> r[c]   — -1/0/1

    // ── Mantıksal ─────────────────────────────────────────────────────────────
    NOT,             // r[a] = !r[b]
    // AND/OR short-circuit: JUMP_IF_FALSE / JUMP_IF_TRUE ile kurulur

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
    // "field index" compile-time'da struct_def.fields[] sırasından hesaplanır.

    // ── Kontrol akışı ─────────────────────────────────────────────────────────
    JUMP,            // ip = a<<8|b             — 16-bit offset
    JUMP_IF_FALSE,   // if !r[a]: ip = b<<8|c
    JUMP_IF_TRUE,    // if  r[a]: ip = b<<8|c
    JUMP_IF_NULL,    // if r[a]==null: ip = b<<8|c

    // ── Fonksiyon çağrısı ─────────────────────────────────────────────────────
    // Çağrı convention:
    //   CALL r_fn, r_args_base, argc
    //   Argümanlar r_args_base .. r_args_base+argc-1 aralığında hazır olmalı.
    //   Sonuç r[a]'ya yazılır.
    CALL,            // r[a] = call r[b](r[b+1]..r[b+c])
    CALL_BUILTIN,    // r[a] = builtins[b](r[c]...) — c=args_base, count in next byte
    TAIL_CALL,       // tail call optimizasyonu — RETURN olmadan CALL
    RETURN,          // return r[a]
    RETURN_NULL,     // return null

    // ── Closure ───────────────────────────────────────────────────────────────
    MAKE_CLOSURE,    // r[a] = Closure{fn_pool[b], captures...}
                     // Sonraki N instruction: CAPTURE_VAL r[x] (derleme ek bilgisi)
    LOAD_CAPTURE,    // r[a] = current_closure.captures[b]
    // STORE_CAPTURE yok — LOOK by-value capture, değiştirme yok

    // ── Iterasyon ─────────────────────────────────────────────────────────────
    FOR_PREP,        // r[a]=iter_state, r[a+1]=0(idx)  — r[b]=array
    FOR_STEP,        // if end: ip=c<<8|d; else r[a+2]=val, r[a+3]=idx, ++r[a+1]
    // foreach ($arr as $v)        → r[a+2]=value
    // foreach ($arr as $k => $v)  → r[a+2]=key, r[a+3]=value

    // ── Hata yönetimi ─────────────────────────────────────────────────────────
    TRY_PUSH,        // catch_stack.push(ip=a<<8|b)    — catch block başlangıcı
    TRY_POP,         // catch_stack.pop()
    THROW,           // throw r[a]
    LOAD_EXC,        // r[a] = current_exception (catch bloğu başında)

    // ── LOOK'a özgü ───────────────────────────────────────────────────────────
    PARALLEL_CALL,   // spawn thread: closure=r[a]     — fire and forget
    CHAN_SEND,        // send(r[a], r[b])
    CHAN_RECV,        // r[a] = receive(r[b])
    CHAN_CLOSE,       // close(r[a])
    CHAN_SIZE,        // r[a] = chan_size(r[b])
    WS_SEND,         // ws::send(r[a], r[b])
    WS_BROADCAST,    // ws::broadcast(r[a])
    SSE_SEND,        // sse::send(r[a], r[b] [, r[c]])
    ROUTE_MATCH,     // dispatcher — internal, tek instruction

    // ── Type / cast ───────────────────────────────────────────────────────────
    TYPE_OF,         // r[a] = type::of(r[b])
    TO_INT,          // r[a] = int(r[b])
    TO_FLOAT,        // r[a] = float(r[b])
    TO_STR,          // r[a] = string(r[b])
    TO_BOOL,         // r[a] = bool(r[b])

    // ── Debug ─────────────────────────────────────────────────────────────────
    NOP,
    BREAKPOINT,      // VM debug hook
};

// ── Instruction ───────────────────────────────────────────────────────────────

struct Instruction {
    OpCode op;
    uint8_t a = 0;
    uint8_t b = 0;
    uint8_t c = 0;

    // Helpers
    static Instruction make(OpCode op, uint8_t a=0, uint8_t b=0, uint8_t c=0) {
        return {op, a, b, c};
    }
    // 16-bit b<<8|c alanı
    uint16_t bx() const { return (uint16_t(b) << 8) | c; }
};

static_assert(sizeof(Instruction) == 4, "Instruction 4 byte olmalı");

// ── Constant pool ─────────────────────────────────────────────────────────────
//
// Per-function. Her sabit literali (sayı, string, bool, null) buraya girer.
// Bir fonksiyon max 65535 sabit taşıyabilir (LOAD_CONST_W 16-bit).

using ConstantPool = std::vector<Value>;

// ── FunctionProto — derleme çıktısı ──────────────────────────────────────────
//
// Her LOOK fonksiyonu (lambda dahil) için üretilir.
// VM çalışırken CallFrame bu proto'ya pointer tutar.

struct FunctionProto {
    std::string                          name;         // debug
    int                                  arity  = 0;   // zorunlu parametre sayısı
    bool                                 variadic = false;
    int                                  reg_count = 0;// max register sayısı (compiler hesaplar)
    ConstantPool                         constants;
    std::vector<Instruction>             code;
    std::vector<std::shared_ptr<FunctionProto>> nested; // iç fonksiyonlar

    // Kaynak konum bilgisi — her instruction için satır numarası
    std::vector<int>                     lines;

    // Parametre isimleri (hata mesajı için)
    std::vector<std::string>             params;
};

// ── Closure object — runtime ──────────────────────────────────────────────────
//
// FunctionProto + capture edilmiş değerler.
// parallel() thread'e taşınırken snapshot alınır — sonradan değişmez.

struct Closure {
    std::shared_ptr<FunctionProto> proto;
    std::vector<Value>             captures; // use ($a,$b,...) sırasıyla

    explicit Closure(std::shared_ptr<FunctionProto> p) : proto(std::move(p)) {}
};

// ── CallFrame — VM çalışma yığını ─────────────────────────────────────────────

struct CallFrame {
    std::shared_ptr<Closure> closure;
    int                      ip      = 0;   // instruction pointer
    int                      base    = 0;   // register array başlangıç offset'i
    int                      ret_reg = 0;   // caller'ın sonucu koyacağı register
};

// ── CompiledProgram — tüm programın bytecode çıktısı ─────────────────────────

struct CompiledProgram {
    std::shared_ptr<FunctionProto> main_proto; // top-level
    // Struct tanımları, route kayıtları setup fazında çalıştırılarak kurulur.
    // Bytecode yorumda StructDef ve route_registry_ mevcut interpreter
    // altyapısını kullanmaya devam eder — Phase 17'de taşınabilir.
};

} // namespace look

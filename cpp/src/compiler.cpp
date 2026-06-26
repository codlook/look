// LOOK Bytecode Compiler — AST → FunctionProto
//
// Her FunctionCompiler bir LOOK fonksiyonu/closure için bytecode üretir.
// Compiler::compile() programın tamamını derler.
//
// 3 kritik tasarım kararı:
//   1. Register allocation: temp'ler free() ile geri verilir, 256 sınırı korunur
//   2. Short-circuit: && ve || JUMP_IF_FALSE/TRUE zinciri ile üretilir
//   3. try/catch: catch bloğuna girerken sadece STORE_VAR yapılanlar geçerli

#include "look/compiler.h"
#include "look/interpreter.h"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace look {

// ── Yardımcı: builtin fonksiyon indeksi ──────────────────────────────────────
//
// CALL_BUILTIN instruction'ı için: string isim → uint8_t index.
// VM tarafında bu indeks bir function pointer dizisine bakacak.
// Şimdilik sabit liste — yeni built-in eklendikçe buraya eklenir.

static const std::vector<std::string> BUILTIN_NAMES = {
    // 0-9: core
    "print", "write", "count", "push", "pop", "str", "int", "float", "bool", "string",
    // 10-15: type check
    "is_null", "is_int", "is_float", "is_string", "is_bool", "is_array",
    // 16-19: channel (special opcodes — CHAN_SEND/RECV — ama fallback için listede)
    "send", "receive", "close", "chan_size",
    // 20-21: json (eski isimlendirme — geriye dönük uyumluluk)
    "json_encode", "json_decode",
    // 22: route — setup'ta closure kaydeder, dispatch'te no-op
    "route",
    // 23-24: json module (scope resolution yolu — json::encode)
    "json::encode", "json::decode",
    // 25-27: response
    "response::status", "response::header", "response::redirect",
    // 28-35: request
    "request::method", "request::get", "request::post", "request::json",
    "request::body", "request::path", "request::ip", "request::param",
    // 36: request::all
    "request::all",
    // 37-42: db
    "db::query", "db::exec", "db::last_id", "db::affected", "db::col", "db::close",
    // 43-47: log
    "log::info", "log::warn", "log::error", "log::debug", "log::query",
    // 48-51: session
    "session::start", "session::get", "session::set", "session::destroy",
    // 52-55: cookie
    "cookie::get", "cookie::set", "cookie::delete", "cookie::has",
    // 56: db::connect (setup'ta çalışır — pool oluşturur, key string döner)
    "db::connect",
    // 57: channel() — LookChannel oluşturur
    "channel",
    // 58: env() — setup'ta da çağrılır (DB DSN oluşturma)
    "env",
    // 59: config()
    "config",
    // 60-69: date::
    "date::now", "date::today", "date::format", "date::parse", "date::add",
    "date::sub", "date::diff", "date::from_timestamp", "date::is_valid", "date::weekday",
    // 70-79: string::
    "string::upper", "string::lower", "string::trim", "string::replace", "string::contains",
    "string::substr", "string::split", "string::join", "string::len", "string::starts_with",
    // 80-89: string:: continued
    "string::ends_with", "string::reverse", "string::repeat", "string::index_of",
    "string::pad_left", "string::pad_right", "string::slugify", "string::random",
    "string::ltrim", "string::rtrim",
    // 90-91: auth::
    "auth::hash", "auth::verify",
    // 92-97: validator::
    "validator::required", "validator::email", "validator::integer",
    "validator::numeric", "validator::min", "validator::max",
    // 98-100: html::
    "html::escape", "html::attr", "html::strip",
    // 101-103: math::
    "math::abs", "math::floor", "math::ceil",
    // 104: date::week (eksik kalmıştı)
    "date::week",
    // 105: date::timestamp
    "date::timestamp",
    // 106-112: file::
    "file::read", "file::put", "file::append", "file::exists",
    "file::remove", "file::size", "file::store",
};

static int builtin_index(const std::string& name) {
    for (int i = 0; i < (int)BUILTIN_NAMES.size(); ++i)
        if (BUILTIN_NAMES[i] == name) return i;
    return -1;
}

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

    // Parametreler ilk local slot'ları alır
    // regs_ params tamamlandıktan sonra kurulur — declare_local içinde
    // Önce locals_ listesini kur, sonra RegisterAllocator'ı
    scope_depth_ = 0;
    for (auto& p : params) {
        LocalVar lv;
        lv.name  = p;
        lv.reg   = (uint8_t)locals_.size();
        lv.depth = 0;
        locals_.push_back(lv);
    }
    regs_ = std::make_unique<RegisterAllocator>((uint8_t)locals_.size());
}

// ── Emit ──────────────────────────────────────────────────────────────────────

int FunctionCompiler::emit(OpCode op, uint8_t a, uint8_t b, uint8_t c) {
    int ip = (int)proto_.code.size();
    proto_.code.push_back(Instruction::make(op, a, b, c));
    proto_.lines.push_back(0); // satır bilgisi sonradan eklenecek
    return ip;
}

int FunctionCompiler::emit_jump(OpCode op, uint8_t cond_reg) {
    // Hedef bilinmiyor — b ve c alanı 0, sonradan patch_jump ile doldurulur
    return emit(op, cond_reg, 0, 0);
}

void FunctionCompiler::patch_jump(int offset, int target) {
    // 16-bit target: b = hi, c = lo
    if (target > 0xFFFF)
        throw LookCompileError("Jump hedefi çok uzak");
    proto_.code[offset].b = (uint8_t)(target >> 8);
    proto_.code[offset].c = (uint8_t)(target & 0xFF);
}

// ── Constant pool ─────────────────────────────────────────────────────────────

uint16_t FunctionCompiler::add_const(Value v) {
    // Deduplicate — Value::operator== public
    for (size_t i = 0; i < proto_.constants.size(); ++i)
        if (proto_.constants[i] == v) return (uint16_t)i;
    if (proto_.constants.size() >= 0xFFFF)
        throw LookCompileError("Constant pool taştı (max 65535)");
    proto_.constants.push_back(v);
    return (uint16_t)(proto_.constants.size() - 1);
}

void FunctionCompiler::emit_load_const(uint8_t dest, Value v, int /*line*/) {
    // Küçük integer optimizasyonu: -128..127 → LOAD_INT
    if (v.type() == Value::INT && v.as_int() >= -128 && v.as_int() <= 127) {
        emit(OpCode::LOAD_INT, dest, (uint8_t)(int8_t)v.as_int());
        return;
    }
    if (v.type() == Value::NONE)  { emit(OpCode::LOAD_NULL,  dest); return; }
    if (v.type() == Value::BOOL) {
        emit(v.as_bool() ? OpCode::LOAD_TRUE : OpCode::LOAD_FALSE, dest);
        return;
    }
    uint16_t idx = add_const(v);
    if (idx < 256) {
        emit(OpCode::LOAD_CONST,   dest, (uint8_t)idx);
    } else {
        emit(OpCode::LOAD_CONST_W, dest, (uint8_t)(idx >> 8), (uint8_t)(idx & 0xFF));
    }
}

// ── Register ──────────────────────────────────────────────────────────────────

uint8_t FunctionCompiler::alloc_temp() {
    return regs_->alloc();
}

void FunctionCompiler::free_temp(uint8_t r) {
    regs_->free(r);
}

// ── Scope ──────────────────────────────────────────────────────────────────────

void FunctionCompiler::push_scope() {
    ++scope_depth_;
}

void FunctionCompiler::pop_scope() {
    // Scope'a ait local'ları listeden çıkar — register'lar serbest kalır
    while (!locals_.empty() && locals_.back().depth == scope_depth_) {
        regs_->free(locals_.back().reg);
        locals_.pop_back();
    }
    --scope_depth_;
}

uint8_t FunctionCompiler::declare_local(const std::string& name, int line) {
    // Aynı scope'ta aynı isim zaten var mı?
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name)
            throw LookCompileError("'" + name + "' bu scope'ta zaten tanımlı", line);
    }
    // Yeni sabit slot — locals her zaman sıralı
    uint8_t slot = regs_->alloc();
    locals_.push_back({name, slot, scope_depth_});
    return slot;
}

FunctionCompiler::VarLoc FunctionCompiler::resolve_var(const std::string& name) {
    // 1. Local
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it)
        if (it->name == name) return {VarKind::LOCAL, it->reg};

    // 2. Capture (use() listesinden gelen)
    for (auto& c : captures_)
        if (c.name == name) return {VarKind::CAPTURE, c.capture_index};

    // 3. Global
    return {VarKind::GLOBAL, 0};
}

// ── compile — entry point ─────────────────────────────────────────────────────

std::shared_ptr<FunctionProto> FunctionCompiler::compile(const BlockStatement& body) {
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

void FunctionCompiler::compile_stmt(const Statement& stmt) {
    if (auto* s = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        // Sonuç kullanılmıyor — geçici register al, hemen bırak
        uint8_t r = compile_expr(*s->expression);
        free_temp(r);
    }
    else if (auto* s = dynamic_cast<const PrintStatement*>(&stmt)) {
        for (auto& e : s->expressions) {
            uint8_t r = compile_expr(*e);
            emit(OpCode::CALL_BUILTIN, r, 0 /*print*/, r); // result reg = arg reg (sonuç atılır)
            free_temp(r);
        }
    }
    else if (auto* s = dynamic_cast<const WriteStatement*>(&stmt)) {
        for (auto& e : s->expressions) {
            uint8_t r = compile_expr(*e);
            emit(OpCode::CALL_BUILTIN, r, 1 /*write*/, r); // result reg = arg reg (sonuç atılır)
            free_temp(r);
        }
    }
    else if (auto* s = dynamic_cast<const ReturnStatement*>(&stmt)) {
        compile_return(*s);
    }
    else if (dynamic_cast<const BreakStatement*>(&stmt)) {
        if (loop_stack_.empty())
            throw LookCompileError("break döngü dışında");
        int p = emit_jump(OpCode::JUMP);
        loop_stack_.top().break_patches.push_back(p);
    }
    else if (dynamic_cast<const ContinueStatement*>(&stmt)) {
        if (loop_stack_.empty())
            throw LookCompileError("continue döngü dışında");
        int target = loop_stack_.top().continue_target;
        if (target >= 0) {
            // Hedef biliniyor (while/for)
            int p = emit(OpCode::JUMP, 0, (uint8_t)(target >> 8), (uint8_t)(target & 0xFF));
            (void)p;
        } else {
            int p = emit_jump(OpCode::JUMP);
            loop_stack_.top().continue_patches.push_back(p);
        }
    }
    else if (auto* s = dynamic_cast<const IfStatement*>(&stmt)) {
        compile_if(*s);
    }
    else if (auto* s = dynamic_cast<const WhileStatement*>(&stmt)) {
        compile_while(*s);
    }
    else if (auto* s = dynamic_cast<const ForStatement*>(&stmt)) {
        compile_for(*s);
    }
    else if (auto* s = dynamic_cast<const ForeachStatement*>(&stmt)) {
        compile_foreach(*s);
    }
    else if (auto* s = dynamic_cast<const TryCatchStatement*>(&stmt)) {
        compile_try(*s);
    }
    else if (auto* s = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        compile_func_decl(*s);
    }
    else if (auto* s = dynamic_cast<const SwitchStatement*>(&stmt)) {
        compile_switch(*s);
    }
    else if (auto* s = dynamic_cast<const ConstBlock*>(&stmt)) {
        compile_const_block(*s);
    }
    else if (auto* s = dynamic_cast<const StructDeclaration*>(&stmt)) {
        compile_struct_decl(*s);
    }
    else if (auto* s = dynamic_cast<const BlockStatement*>(&stmt)) {
        compile_block(*s);
    }
    else if (dynamic_cast<const UseStatement*>(&stmt)) {
        // use math; — modül yükleme VM'de setup fazında yapılır, bytecode'da NOP
        emit(OpCode::NOP);
    }
    else {
        throw LookCompileError("Bilinmeyen statement tipi: " + std::string(typeid(stmt).name()));
    }
}

// ── compile_return ────────────────────────────────────────────────────────────

void FunctionCompiler::compile_return(const ReturnStatement& s) {
    if (!s.expression) {
        emit(OpCode::RETURN_NULL);
        return;
    }
    uint8_t r = compile_expr(*s.expression);
    emit(OpCode::RETURN, r);
    free_temp(r);
}

// ── compile_if ────────────────────────────────────────────────────────────────

void FunctionCompiler::compile_if(const IfStatement& s) {
    uint8_t cond = compile_expr(*s.condition);
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

    uint8_t cond = compile_expr(*s.condition);
    int exit_jump = emit_jump(OpCode::JUMP_IF_FALSE, cond);
    free_temp(cond);

    loop_stack_.push({.continue_target = loop_start});
    compile_block(*s.body);
    auto ctx = loop_stack_.top();
    loop_stack_.pop();

    // continue → loop_start
    for (int p : ctx.continue_patches) patch_jump(p, loop_start);
    // döngüye geri dön
    emit(OpCode::JUMP, 0, (uint8_t)(loop_start >> 8), (uint8_t)(loop_start & 0xFF));

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
        uint8_t cond = compile_expr(*s.condition);
        exit_jump = emit_jump(OpCode::JUMP_IF_FALSE, cond);
        free_temp(cond);
    }

    loop_stack_.push({.continue_target = -1}); // continue hedefi post sonrası
    compile_block(*s.body);
    auto ctx = loop_stack_.top();
    loop_stack_.pop();

    int post_ip = current_ip();
    if (s.post) {
        uint8_t r = compile_expr(*s.post);
        free_temp(r);
    }

    // continue → post
    for (int p : ctx.continue_patches) patch_jump(p, post_ip);
    emit(OpCode::JUMP, 0, (uint8_t)(loop_start >> 8), (uint8_t)(loop_start & 0xFF));

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

    uint8_t r_arr = compile_expr(*s.iterable);
    // alloc_seq(4): r_iter, r_iter+1(idx), r_iter+2(val), r_iter+3(key)
    // FOR_STEP bu 4 register'a doğrudan yazar; loop body'deki alloc_temp()
    // çağrıları bu bloğa girmemeli — alloc_seq bunu garanti eder.
    uint8_t r_iter = regs_->alloc_seq(4);
    emit(OpCode::FOR_PREP, r_iter, r_arr);
    free_temp(r_arr);

    // value ve key register'ları r_iter+2 ve r_iter+3 — FOR_STEP convention
    uint8_t r_val = r_iter + 2;
    uint8_t r_key = r_iter + 3;

    // Manuel local kayıt — declare_local kullanmıyoruz (slot sabit)
    if (!s.value_var.empty()) locals_.push_back({s.value_var, r_val, scope_depth_});
    if (!s.key_var.empty())   locals_.push_back({s.key_var,   r_key, scope_depth_});

    int loop_start = current_ip();
    // FOR_STEP: a=r_iter, b=exit_hi, c=exit_lo (sonradan patch)
    int step_ip = emit(OpCode::FOR_STEP, r_iter, 0, 0);

    loop_stack_.push({.continue_target = loop_start});
    compile_block(*s.body);
    auto ctx = loop_stack_.top();
    loop_stack_.pop();

    for (int p : ctx.continue_patches) patch_jump(p, loop_start);
    emit(OpCode::JUMP, 0, (uint8_t)(loop_start >> 8), (uint8_t)(loop_start & 0xFF));

    int after = current_ip();
    // FOR_STEP exit patch: b ve c alanına after yaz
    proto_.code[step_ip].b = (uint8_t)(after >> 8);
    proto_.code[step_ip].c = (uint8_t)(after & 0xFF);

    for (int p : ctx.break_patches) patch_jump(p, after);

    // alloc_seq(4) ile alındı — 4 register birden serbest bırak
    for (int k = 0; k < 4; ++k) regs_->free(r_iter + k);
    pop_scope();
}

// ── compile_try ───────────────────────────────────────────────────────────────
//
// Kritik 3. nokta: catch bloğuna girerken sadece STORE_VAR olanlar geçerli.
// Temp register'lar try bloğu bitince serbest bırakılmış olur (normal akış).
// catch bloğu ayrı scope — try içindeki temp'ler zaten geri verilmiş.

void FunctionCompiler::compile_try(const TryCatchStatement& s) {
    // TRY_PUSH: catch IP'si sonradan patch edilecek (b,c = catch_ip)
    int try_push_ip = emit(OpCode::TRY_PUSH, 0, 0, 0);

    compile_block(*s.try_block);

    emit(OpCode::TRY_POP);

    // try başarılı → catch'i atla
    int jump_end = emit_jump(OpCode::JUMP);

    // catch bloğu
    int catch_ip = current_ip();
    patch_jump(try_push_ip, catch_ip); // TRY_PUSH'ın b,c alanına catch_ip

    if (s.catch_block) {
        push_scope();
        // catch değişkeni ($e) — exception değeri LOAD_EXC ile gelir
        if (!s.catch_var.empty()) {
            uint8_t e_reg = declare_local(s.catch_var, 0);
            emit(OpCode::LOAD_EXC, e_reg);
        }
        compile_block(*s.catch_block);
        pop_scope();
    }

    int after_catch = current_ip();
    patch_jump(jump_end, after_catch);

    // finally — her iki daldan sonra çalışır
    if (s.finally_block) {
        compile_block(*s.finally_block);
    }
}

// ── compile_func_decl ─────────────────────────────────────────────────────────

void FunctionCompiler::compile_func_decl(const FunctionDeclaration& s) {
    // İç fonksiyon derle
    FunctionCompiler inner(s.name, s.parameters, s.is_variadic, this);
    auto proto = inner.compile(*s.body);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    // Fonksiyon adı global scope'a kaydedilir
    uint16_t name_idx = add_const(Value(s.name));
    uint8_t  r        = alloc_temp();
    // MAKE_CLOSURE: a=r, b=fn_idx (nested index)
    emit(OpCode::MAKE_CLOSURE, r, (uint8_t)fn_idx);
    // Global'e kaydet — top-level function declaration (16-bit const index)
    emit(OpCode::STORE_GLOBAL, r, (uint8_t)(name_idx >> 8), (uint8_t)(name_idx & 0xFF));
    free_temp(r);
}

// ── compile_switch ────────────────────────────────────────────────────────────
//
// Go stili — her case otomatik break, fallthrough yok.

void FunctionCompiler::compile_switch(const SwitchStatement& s) {
    uint8_t subj = compile_expr(*s.subject);

    std::vector<int> case_jumps;   // her case'in başına jump
    std::vector<int> end_jumps;    // her case sonunda switch dışına jump

    // Her case için: koşul kontrol et → eşleşmezse next case'e atla
    std::vector<int> body_ips;
    std::vector<int> default_ip_holder; // default case IP

    // İlk pass: condition check'ler
    struct CaseEntry { std::vector<int> cond_jumps; bool is_default; };
    std::vector<CaseEntry> entries;

    for (auto& c : s.cases) {
        CaseEntry entry;
        entry.is_default = c.values.empty();
        if (!entry.is_default) {
            for (auto& v : c.values) {
                uint8_t r = compile_expr(*v);
                uint8_t eq = alloc_temp();
                emit(OpCode::EQ, eq, subj, r);
                free_temp(r);
                // EQ true → bu case'e gir
                entry.cond_jumps.push_back(emit_jump(OpCode::JUMP_IF_TRUE, eq));
                free_temp(eq);
            }
        }
        entries.push_back(std::move(entry));
    }

    // Hiçbir case eşleşmedi → default veya sona
    int no_match_jump = emit_jump(OpCode::JUMP);

    // İkinci pass: body'ler
    for (size_t i = 0; i < s.cases.size(); ++i) {
        auto& c    = s.cases[i];
        auto& entry = entries[i];
        int body_ip = current_ip();

        if (entry.is_default) {
            // default case: no_match_jump buraya gelir
            patch_jump(no_match_jump, body_ip);
        } else {
            for (int j : entry.cond_jumps) patch_jump(j, body_ip);
        }

        push_scope();
        for (auto& stmt : c.body) compile_stmt(*stmt);
        pop_scope();
        end_jumps.push_back(emit_jump(OpCode::JUMP));
    }

    int after = current_ip();

    // default yoksa no_match_jump → after
    bool has_default = false;
    for (auto& e : entries) if (e.is_default) { has_default = true; break; }
    if (!has_default) patch_jump(no_match_jump, after);

    for (int p : end_jumps) patch_jump(p, after);
    free_temp(subj);
}

// ── compile_const_block ───────────────────────────────────────────────────────

void FunctionCompiler::compile_const_block(const ConstBlock& s) {
    iota_val_ = 0;
    // iota_expr_: ilk iota içeren expression — sonraki satırlar aynı ifadeyi tekrar eder
    const Expression* last_iota_expr = nullptr;

    for (auto& item : s.items) {
        Value v;
        if (!item.value) {
            // Önceki iota ifadesini yeniden değerlendir (iota değeri artmış)
            // Compiler basitleştirmesi: iota_ doğrusal → sadece sayısal destekle
            v = Value(iota_val_++);
        } else if (auto* iota = dynamic_cast<const IotaExpression*>(item.value.get())) {
            v = Value(iota_val_++);
            last_iota_expr = nullptr;
        } else {
            // Sabit expression — compile-time eval girişimi
            // Basit literaller için direkt değer al
            if (auto* n = dynamic_cast<const NumberLiteral*>(item.value.get())) {
                v = Value(n->value);
            } else if (auto* f = dynamic_cast<const FloatLiteral*>(item.value.get())) {
                v = Value(f->value);
            } else if (auto* str = dynamic_cast<const StringLiteral*>(item.value.get())) {
                v = Value(str->value);
            } else if (auto* b = dynamic_cast<const BooleanLiteral*>(item.value.get())) {
                v = Value(b->value);
            } else {
                // Karmaşık expression: runtime'da değerlendir, global'e store et
                uint8_t r = compile_expr(*item.value);
                uint16_t name_idx = add_const(Value(item.name));
                emit(OpCode::STORE_GLOBAL, r, (uint8_t)(name_idx >> 8), (uint8_t)(name_idx & 0xFF));
                free_temp(r);
                continue;
            }
        }
        // Sabit değeri global'e yükle
        uint8_t r = alloc_temp();
        emit_load_const(r, v, 0);
        uint16_t name_idx = add_const(Value(item.name));
        emit(OpCode::STORE_GLOBAL, r, (uint8_t)(name_idx >> 8), (uint8_t)(name_idx & 0xFF));
        free_temp(r);
    }
}

// ── compile_struct_decl ───────────────────────────────────────────────────────

void FunctionCompiler::compile_struct_decl(const StructDeclaration& s) {
    // Struct tanımı runtime'da işlenir — mevcut interpreter altyapısı korunuyor
    // VM'e STRUCT_DEF instruction gönderilir; vm.cpp bunu StructDef tablosuna kaydeder
    uint16_t name_idx = add_const(Value(s.name));
    emit(OpCode::NEW_STRUCT, 0, (uint8_t)(name_idx >> 8), (uint8_t)(name_idx & 0xFF));
    // Field isimleri: her biri LOAD_CONST + STORE_GLOBAL olarak sıralanır
    // Detay: vm.cpp NEW_STRUCT handler'ı sonraki N LOAD_CONST instruction'ı okur
    // Bu yaklaşım struct tanımını bytecode'a gömer — değişmez
    uint8_t field_count = (uint8_t)s.fields.size();
    emit(OpCode::NOP, field_count); // field sayısı hint — vm.cpp okur
    for (auto& f : s.fields) {
        uint16_t fi = add_const(Value(f.name));
        emit(OpCode::LOAD_CONST_W, 0, (uint8_t)(fi >> 8), (uint8_t)(fi & 0xFF));
    }
}

// ── compile_assign_expr ───────────────────────────────────────────────────────

void FunctionCompiler::compile_assign_expr(const AssignmentExpression& e) {
    auto loc = resolve_var(e.name);

    if (e.index) {
        // $arr[i] = val
        uint8_t arr  = alloc_temp();
        if      (loc.kind == VarKind::LOCAL)   emit(OpCode::MOVE,        arr, loc.index);
        else if (loc.kind == VarKind::CAPTURE) emit(OpCode::LOAD_CAPTURE, arr, loc.index);
        else {
            uint16_t ni = add_const(Value(e.name));
            emit(OpCode::LOAD_GLOBAL, arr, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        }
        uint8_t idx = compile_expr(*e.index);
        uint8_t val = compile_expr(*e.value);
        emit(OpCode::ARRAY_SET, arr, idx, val);
        free_temp(val); free_temp(idx); free_temp(arr);
        return;
    }

    uint8_t val = compile_expr(*e.value);

    if (loc.kind == VarKind::LOCAL) {
        // Compound assign için mevcut değeri oku
        if (e.op != "=") {
            uint8_t cur = alloc_temp();
            emit(OpCode::MOVE, cur, loc.index);
            uint8_t tmp = alloc_temp();
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD}, {"-=", OpCode::SUB}, {"*=", OpCode::MUL},
                {"/=", OpCode::DIV}, {"%=", OpCode::MOD}, {".=", OpCode::CONCAT},
            };
            auto it = COMPOUND.find(e.op);
            if (it == COMPOUND.end()) throw LookCompileError("Bilinmeyen compound op: " + e.op);
            emit(it->second, tmp, cur, val);
            free_temp(cur); free_temp(val);
            emit(OpCode::MOVE, loc.index, tmp);
            free_temp(tmp);
        } else {
            emit(OpCode::MOVE, loc.index, val);
            free_temp(val);
        }
    } else if (loc.kind == VarKind::CAPTURE) {
        // LOOK by-value capture — capture değiştirilemiyor (felsefe)
        throw LookCompileError("Capture edilen değişken değiştirilemez: $" + e.name);
    } else {
        // Global
        uint16_t ni = add_const(Value(e.name));
        emit(OpCode::STORE_GLOBAL, val, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        free_temp(val);
    }
}

// ── compile_expr — dispatch ───────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_expr(const Expression& expr, uint8_t dest) {
    auto ensure_dest = [&]() -> uint8_t {
        return (dest == 255) ? alloc_temp() : dest;
    };

    if (auto* e = dynamic_cast<const NumberLiteral*>(&expr)) {
        uint8_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (auto* e = dynamic_cast<const FloatLiteral*>(&expr)) {
        uint8_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (auto* e = dynamic_cast<const BooleanLiteral*>(&expr)) {
        uint8_t r = ensure_dest();
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }
    if (dynamic_cast<const NullLiteral*>(&expr)) {
        uint8_t r = ensure_dest();
        emit(OpCode::LOAD_NULL, r);
        return r;
    }
    if (auto* e = dynamic_cast<const StringLiteral*>(&expr)) {
        uint8_t r = ensure_dest();
        // Interpolation içeriyor mu? Lexer raw string gönderir
        // Basit string (interpolation yok) → LOAD_CONST
        // İnterpolasyon → compile_string_interp
        if (e->value.find("{$") != std::string::npos) {
            return compile_string_interp(e->value, expr.loc.line, r);
        }
        emit_load_const(r, Value(e->value), expr.loc.line);
        return r;
    }

    if (auto* e = dynamic_cast<const Variable*>(&expr)) {
        auto loc = resolve_var(e->name);
        if (loc.kind == VarKind::LOCAL) {
            if (dest == 255) return loc.index; // direkt slot — kopyalamaya gerek yok
            emit(OpCode::MOVE, dest, loc.index);
            return dest;
        }
        uint8_t r = ensure_dest();
        if (loc.kind == VarKind::CAPTURE) {
            emit(OpCode::LOAD_CAPTURE, r, loc.index);
        } else {
            uint16_t ni = add_const(Value(e->name));
            emit(OpCode::LOAD_GLOBAL, r, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        }
        return r;
    }

    if (auto* e = dynamic_cast<const AssignmentExpression*>(&expr)) {
        compile_assign_expr(*e);
        // Atama expression olarak kullanılmış — değeri oku
        auto loc = resolve_var(e->name);
        uint8_t r = ensure_dest();
        if (loc.kind == VarKind::LOCAL) emit(OpCode::MOVE, r, loc.index);
        else {
            uint16_t ni = add_const(Value(e->name));
            emit(OpCode::LOAD_GLOBAL, r, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        }
        return r;
    }

    if (auto* e = dynamic_cast<const BinaryExpression*>(&expr)) {
        return compile_binary(*e, dest);
    }

    if (auto* e = dynamic_cast<const UnaryExpression*>(&expr)) {
        uint8_t operand = compile_expr(*e->right);
        uint8_t r = ensure_dest();
        if (e->op == "-")  emit(OpCode::UNM, r, operand);
        else if (e->op == "!") emit(OpCode::NOT, r, operand);
        else if (e->op == "~") emit(OpCode::BNOT, r, operand);
        else throw LookCompileError("Bilinmeyen unary op: " + e->op);
        free_temp(operand);
        return r;
    }

    if (auto* e = dynamic_cast<const TernaryExpression*>(&expr)) {
        uint8_t cond = compile_expr(*e->condition);
        int jf = emit_jump(OpCode::JUMP_IF_FALSE, cond);
        free_temp(cond);
        uint8_t r = ensure_dest();
        uint8_t then_r = compile_expr(*e->then_expr, r);
        if (then_r != r) { emit(OpCode::MOVE, r, then_r); free_temp(then_r); }
        int jend = emit_jump(OpCode::JUMP);
        patch_jump(jf, current_ip());
        uint8_t else_r = compile_expr(*e->else_expr, r);
        if (else_r != r) { emit(OpCode::MOVE, r, else_r); free_temp(else_r); }
        patch_jump(jend, current_ip());
        return r;
    }

    if (auto* e = dynamic_cast<const CallExpression*>(&expr)) {
        return compile_call(*e, dest);
    }

    if (auto* e = dynamic_cast<const FunctionExpression*>(&expr)) {
        return compile_closure(*e, dest);
    }

    if (auto* e = dynamic_cast<const ArrayLiteral*>(&expr)) {
        return compile_array_lit(*e, dest);
    }

    if (auto* e = dynamic_cast<const AssocArrayLiteral*>(&expr)) {
        return compile_assoc_lit(*e, dest);
    }

    if (auto* e = dynamic_cast<const StructLiteralExpression*>(&expr)) {
        return compile_struct_lit(*e, dest);
    }

    if (auto* e = dynamic_cast<const IndexExpression*>(&expr)) {
        uint8_t obj = compile_expr(*e->object);
        uint8_t idx = compile_expr(*e->index);
        uint8_t r   = ensure_dest();
        emit(OpCode::ARRAY_GET, r, obj, idx);
        free_temp(idx); free_temp(obj);
        return r;
    }

    if (auto* e = dynamic_cast<const MemberAccessExpression*>(&expr)) {
        uint8_t obj = compile_expr(*e->object);
        uint8_t r   = ensure_dest();
        // Field ismi constant pool'a girer — runtime'da field index çözülür
        uint16_t fi = add_const(Value(e->field));
        emit(OpCode::GET_FIELD, r, obj, (uint8_t)(fi & 0xFF));
        free_temp(obj);
        return r;
    }

    if (auto* e = dynamic_cast<const ScopeResolution*>(&expr)) {
        // module::func → LOAD_GLOBAL "module::func"
        std::string full = e->module_name + "::" + e->member_name;
        uint8_t r = ensure_dest();
        uint16_t ni = add_const(Value(full));
        emit(OpCode::LOAD_GLOBAL, r, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        return r;
    }

    throw LookCompileError("Bilinmeyen expression tipi: " + std::string(typeid(expr).name()),
                           expr.loc.line);
}

// ── compile_binary ────────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_binary(const BinaryExpression& e, uint8_t dest) {
    // Kritik 2. nokta: && ve || short-circuit
    if (e.op == "&&" || e.op == "||") return compile_logical(e, dest);

    // Null coalescing: a ?? b → a if a is not null, else b
    if (e.op == "??") {
        uint8_t lhs = compile_expr(*e.left);
        uint8_t r = (dest == 255) ? alloc_temp() : dest;
        // Jump to rhs-block if lhs IS null
        int jnull = emit_jump(OpCode::JUMP_IF_NULL, lhs);
        // lhs is NOT null → use lhs
        emit(OpCode::MOVE, r, lhs);
        free_temp(lhs);
        int jend = emit_jump(OpCode::JUMP);
        // lhs IS null → evaluate rhs
        patch_jump(jnull, current_ip());
        uint8_t rhs = compile_expr(*e.right, r);
        if (rhs != r) { emit(OpCode::MOVE, r, rhs); free_temp(rhs); }
        patch_jump(jend, current_ip());
        return r;
    }

    uint8_t l = compile_expr(*e.left);
    uint8_t r2 = compile_expr(*e.right);
    uint8_t r  = (dest == 255) ? alloc_temp() : dest;

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
    auto it = OPS.find(e.op);
    if (it == OPS.end())
        throw LookCompileError("Bilinmeyen binary op: " + e.op, e.loc.line);

    emit(it->second, r, l, r2);
    free_temp(r2); free_temp(l);
    return r;
}

// ── compile_logical — short-circuit && / || ────────────────────────────────────

uint8_t FunctionCompiler::compile_logical(const BinaryExpression& e, uint8_t dest) {
    uint8_t r = (dest == 255) ? alloc_temp() : dest;

    uint8_t lhs = compile_expr(*e.left, r);
    if (lhs != r) { emit(OpCode::MOVE, r, lhs); free_temp(lhs); }

    int short_circuit;
    if (e.op == "&&") {
        // lhs false → sonuç false, rhs değerlendirilmez
        short_circuit = emit_jump(OpCode::JUMP_IF_FALSE, r);
    } else {
        // lhs true → sonuç true, rhs değerlendirilmez
        short_circuit = emit_jump(OpCode::JUMP_IF_TRUE, r);
    }

    uint8_t rhs = compile_expr(*e.right, r);
    if (rhs != r) { emit(OpCode::MOVE, r, rhs); free_temp(rhs); }

    patch_jump(short_circuit, current_ip());
    return r;
}

// ── compile_call ──────────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_call(const CallExpression& e, uint8_t dest) {
    // Özel fonksiyonlar: parallel, send, receive
    if (auto* sr = dynamic_cast<const ScopeResolution*>(e.callee.get())) {
        std::string full = sr->module_name + "::" + sr->member_name;
        int bidx = builtin_index(full);
        if (bidx >= 0) {
            // Bilinen modül fonksiyonu → CALL_BUILTIN
            // alloc_seq ile ardışık register bloğu al — VM base+k varsayımına uyar
            uint8_t argc = (uint8_t)e.arguments.size();
            uint8_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
            for (int k = 0; k < argc; ++k) {
                uint8_t ev = compile_expr(*e.arguments[k], base + k);
                if (ev != base + k) emit(OpCode::MOVE, base + k, ev);
            }
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CALL_BUILTIN, r, (uint8_t)bidx, base);
            emit(OpCode::NOP, argc);   // VM argc hint
            for (int k = 0; k < argc; ++k) regs_->free(base + k);
            return r;
        }
        // Bilinmeyen modül fonksiyonu → genel CALL yolu
    }
    if (auto* var = dynamic_cast<const Variable*>(e.callee.get())) {
        if (var->name == "parallel") {
            if (e.arguments.size() != 1)
                throw LookCompileError("parallel() tek argüman alır", e.loc.line);
            uint8_t closure = compile_expr(*e.arguments[0]);
            emit(OpCode::PARALLEL_CALL, closure);
            free_temp(closure);
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "send") {
            if (e.arguments.size() != 2)
                throw LookCompileError("send() 2 argüman alır", e.loc.line);
            uint8_t ch  = compile_expr(*e.arguments[0]);
            uint8_t val = compile_expr(*e.arguments[1]);
            emit(OpCode::CHAN_SEND, ch, val);
            free_temp(val); free_temp(ch);
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "receive") {
            if (e.arguments.size() != 1)
                throw LookCompileError("receive() 1 argüman alır", e.loc.line);
            uint8_t ch = compile_expr(*e.arguments[0]);
            uint8_t r  = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CHAN_RECV, r, ch);
            free_temp(ch);
            return r;
        }
        if (var->name == "close") {
            uint8_t ch = compile_expr(*e.arguments[0]);
            emit(OpCode::CHAN_CLOSE, ch);
            free_temp(ch);
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "chan_size") {
            uint8_t ch = compile_expr(*e.arguments[0]);
            uint8_t r  = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CHAN_SIZE, r, ch);
            free_temp(ch);
            return r;
        }
        // Bilinen built-in?
        int bidx = builtin_index(var->name);
        if (bidx >= 0) {
            // alloc_seq ile ardışık register bloğu al — VM base+k varsayımına uyar
            uint8_t argc = (uint8_t)e.arguments.size();
            uint8_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
            for (int k = 0; k < argc; ++k) {
                uint8_t ev = compile_expr(*e.arguments[k], base + k);
                if (ev != base + k) emit(OpCode::MOVE, base + k, ev);
            }
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CALL_BUILTIN, r, (uint8_t)bidx, base);
            emit(OpCode::NOP, argc);   // VM argc hint
            for (int k = 0; k < argc; ++k) regs_->free(base + k);
            return r;
        }
    }

    // Genel CALL: callee'yi register'a al, argümanları sıraya diz
    uint8_t fn = compile_expr(*e.callee);
    // alloc_seq ile ardışık register bloğu al — VM base+k varsayımına uyar
    uint8_t argc = (uint8_t)e.arguments.size();
    uint8_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
    for (int k = 0; k < argc; ++k) {
        uint8_t ev = compile_expr(*e.arguments[k], base + k);
        if (ev != base + k) emit(OpCode::MOVE, base + k, ev);
    }
    uint8_t r = (dest == 255) ? alloc_temp() : dest;
    emit(OpCode::CALL, r, fn, base);
    // argc ek byte olarak NOP'a gömülür — VM okur
    emit(OpCode::NOP, argc);
    for (int k = 0; k < argc; ++k) regs_->free(base + k);
    free_temp(fn);
    return r;
}

// ── compile_closure ───────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_closure(const FunctionExpression& e, uint8_t dest) {
    // use() listesindeki capture'ları çözümle
    // Her capture: parent'tan değer al → Closure.captures[]'e snapshot
    FunctionCompiler inner("<closure>", e.parameters, e.is_variadic, this);

    // Capture mapping'i inner'a bildir
    for (size_t i = 0; i < e.captures.size(); ++i) {
        inner.captures_.push_back({e.captures[i], (uint8_t)i});
    }

    auto proto = inner.compile(*e.body);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    uint8_t r = (dest == 255) ? alloc_temp() : dest;

    // Capture değerlerini MAKE_CLOSURE'dan ÖNCE yükle — VM MAKE_CLOSURE'dan hemen
    // sonra art arda LOAD_CAPTURE(0, cr) hint'lerini bekler; araya MOVE giremez.
    std::vector<uint8_t> cap_regs;
    for (auto& cap_name : e.captures) {
        auto loc = resolve_var(cap_name);
        uint8_t cr = alloc_temp();
        if      (loc.kind == VarKind::LOCAL)   emit(OpCode::MOVE,        cr, loc.index);
        else if (loc.kind == VarKind::CAPTURE) emit(OpCode::LOAD_CAPTURE, cr, loc.index);
        else {
            uint16_t ni = add_const(Value(cap_name));
            emit(OpCode::LOAD_GLOBAL, cr, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
        }
        cap_regs.push_back(cr);
    }

    emit(OpCode::MAKE_CLOSURE, r, (uint8_t)fn_idx);

    // MAKE_CLOSURE'dan hemen sonra art arda hint — VM bu pattern'ı okur
    for (uint8_t cr : cap_regs) {
        emit(OpCode::LOAD_CAPTURE, 0, cr);
        free_temp(cr);
    }
    return r;
}

// ── compile_string_interp ─────────────────────────────────────────────────────
//
// "{$name} merhaba {$x+$y}" → CONCAT zinciri
// Basit implementasyon: interpreter string interpolation mantığını yeniden kullanır.
// TODO: tam expression parser — şimdilik {$var} ve sabit parçalar destekleniyor

uint8_t FunctionCompiler::compile_string_interp(const std::string& raw, int line, uint8_t dest) {
    // Interpolation olmayan string → direkt yükle
    if (raw.find('{') == std::string::npos) {
        uint8_t r = (dest == 255) ? alloc_temp() : dest;
        emit_load_const(r, Value(raw), line);
        return r;
    }

    // Parçalara böl: sabit string + değişken adı dönüşümlü
    // CALL_BUILTIN TO_STR + CONCAT ile birleştir
    std::vector<uint8_t> parts;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '{' && i + 1 < raw.size() && raw[i+1] == '$') {
            size_t end = raw.find('}', i);
            if (end == std::string::npos) break;
            std::string expr_str = raw.substr(i + 1, end - i - 1);
            // $var erişimi
            if (!expr_str.empty() && expr_str[0] == '$') {
                std::string var_name = expr_str.substr(1);
                uint8_t pr = alloc_temp();
                auto loc = resolve_var(var_name);
                if (loc.kind == VarKind::LOCAL) {
                    emit(OpCode::MOVE, pr, loc.index);
                } else if (loc.kind == VarKind::CAPTURE) {
                    emit(OpCode::LOAD_CAPTURE, pr, loc.index);
                } else {
                    uint16_t ni = add_const(Value(var_name));
                    emit(OpCode::LOAD_GLOBAL, pr, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
                }
                emit(OpCode::TO_STR, pr, pr);
                parts.push_back(pr);
            }
            i = end + 1;
        } else {
            // {$ olmayan bölüm — sonraki {$ ye kadar literal
            size_t end = raw.find("{$", i);
            std::string literal = (end == std::string::npos) ? raw.substr(i) : raw.substr(i, end - i);
            if (!literal.empty()) {
                uint8_t pr = alloc_temp();
                emit_load_const(pr, Value(literal), line);
                parts.push_back(pr);
            }
            i = (end == std::string::npos) ? raw.size() : end;
        }
    }

    if (parts.empty()) {
        uint8_t r = (dest == 255) ? alloc_temp() : dest;
        emit(OpCode::LOAD_NULL, r);
        return r;
    }
    if (parts.size() == 1) {
        if (dest != 255 && parts[0] != dest) { emit(OpCode::MOVE, dest, parts[0]); free_temp(parts[0]); return dest; }
        return parts[0];
    }

    // CONCAT zinciri: r = parts[0] . parts[1] . ...
    uint8_t r = (dest == 255) ? alloc_temp() : dest;
    emit(OpCode::CONCAT, r, parts[0], parts[1]);
    free_temp(parts[0]); free_temp(parts[1]);
    for (size_t j = 2; j < parts.size(); ++j) {
        emit(OpCode::CONCAT, r, r, parts[j]);
        free_temp(parts[j]);
    }
    return r;
}

// ── compile_array_lit ─────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_array_lit(const ArrayLiteral& e, uint8_t dest) {
    uint8_t r = (dest == 255) ? alloc_temp() : dest;
    emit(OpCode::NEW_ARRAY, r, (uint8_t)e.elements.size());
    for (auto& el : e.elements) {
        uint8_t er = compile_expr(*el);
        emit(OpCode::ARRAY_PUSH, r, er);
        free_temp(er);
    }
    return r;
}

// ── compile_assoc_lit ─────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_assoc_lit(const AssocArrayLiteral& e, uint8_t dest) {
    uint8_t r = (dest == 255) ? alloc_temp() : dest;
    emit(OpCode::NEW_ASSOC, r);
    for (auto& [k, v] : e.pairs) {
        uint8_t kr = compile_expr(*k);
        uint8_t vr = compile_expr(*v);
        emit(OpCode::ARRAY_SET, r, kr, vr);
        free_temp(vr); free_temp(kr);
    }
    return r;
}

// ── compile_struct_lit ────────────────────────────────────────────────────────

uint8_t FunctionCompiler::compile_struct_lit(const StructLiteralExpression& e, uint8_t dest) {
    uint8_t r = (dest == 255) ? alloc_temp() : dest;
    uint16_t ni = add_const(Value(e.struct_name));
    emit(OpCode::NEW_STRUCT, r, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
    for (auto& [fname, fval] : e.fields) {
        uint16_t fi = add_const(Value(fname));
        uint8_t  vr = compile_expr(*fval);
        emit(OpCode::SET_FIELD, r, (uint8_t)(fi & 0xFF), vr);
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

CompiledProgram Compiler::compile(const Program& program) {
    FunctionCompiler main_compiler("<main>", {}, false, nullptr);
    auto proto = main_compiler.compile_stmts(program.statements);

    CompiledProgram out;
    out.main_proto = proto;
    return out;
}

} // namespace look

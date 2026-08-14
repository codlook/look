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
#include "look/builtins.h"
#include "look/interpreter.h"
#include "look/lexer.h"
#include "look/parser.h"

#include <fstream>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace look {

// ── Builtin indeksi — TEK kaynak: look/builtins.h (src/builtins.cpp) ─────────
//
// CALL_BUILTIN instruction'ı için: string isim → index.
// Liste buradan src/builtins.cpp'ye TAŞINDI — http_main.cpp wiring aynı
// tablodan beslenir, iki dosyanın elle senkron tutulması gerekmez
// (strlen bug'ının kökü bu senkron kopukluğuydu).


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
    proto_.lines.push_back(cur_line_); // satır tablosu — VM hata konumu için (compile_stmt doldurur)
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
        throw LookCompileError("Constant pool overflow (max 65535)");
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
        regs_->free_local(locals_.back().reg);
        locals_.pop_back();
    }
    --scope_depth_;
}

uint8_t FunctionCompiler::declare_local(const std::string& name, int line) {
    // Aynı scope'ta aynı isim zaten var mı?
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name)
            throw LookCompileError("'" + name + "' already defined in this scope", line);
    }
    // Yeni korumalı local slot — free() bunu havuza atmaz (aksi halde compile_expr'in
    // "local slot'unu doğrudan döndür" optimizasyonu + free_temp local'i bozardı).
    uint8_t slot = regs_->alloc_local();
    locals_.push_back({name, slot, scope_depth_});
    // 58 Adım 2a: keşif-geçişi bu ismi "kaçan local" bulduysa slot'unu boxed işaretle.
    // (Adım 2b'de helper'lar boxed slot'lar için cell_get/cell_set yayacak.)
    if (boxed_names_.count(name)) boxed_slots_.insert(slot);
    return slot;
}

FunctionCompiler::VarLoc FunctionCompiler::resolve_var(const std::string& name, bool for_write) {
    // 1. Local
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it)
        if (it->name == name) return {VarKind::LOCAL, it->reg};

    // 2. Capture (use() listesinden VEYA daha önce otomatik yakalanan)
    for (auto& c : captures_)
        if (c.name == name) return {VarKind::CAPTURE, c.capture_index};

    // 3. Otomatik (implicit) capture — OKUMA'da: isim dış fonksiyonun local/capture'ı
    //    ise closure'a by-value snapshot olarak yakala. Eskiden GLOBAL'e düşüyordu →
    //    `$m=3; array::map($a, fn($x)=>$x*$m)` VM'de $m'i göremeyip 0 veriyordu
    //    (interpreter lexical scope ile 3 görüyordu — sessiz divergence). YAZMA'da
    //    yakalamayız: dış değişkene atama yeni local yaratır (Python-benzeri, mevcut
    //    davranış korunur; capture'lar zaten değiştirilemez).
    if (!for_write && parent_) {
        VarLoc pl = parent_->resolve_var(name, /*for_write=*/false);
        if (pl.kind != VarKind::GLOBAL) {
            uint8_t idx = (uint8_t)captures_.size();
            // 58: parent'ta bu isim boxed local (veya boxed capture) ise, yakalanan
            // şey CELL'dir → closure gövdesi okurken [0] deref etmeli (by-ref).
            bool is_cell = parent_->is_cell_var(pl);
            captures_.push_back({name, idx, is_cell});
            return {VarKind::CAPTURE, idx};
        }
    }

    // 4. Global
    return {VarKind::GLOBAL, 0};
}

// ── Local erişim helper'ları (58. bug closure fix hazırlığı) ──────────────────
// ŞU AN davranış-değişmez: düz MOVE. Sonraki adımda "boxed" (cell) local'ler için
// bu iki nokta cell_get/cell_set yayacak — tüm local erişimi buradan geçtiği için
// boxing kararı TEK yere lokalize olur (dağınık değil).
void FunctionCompiler::emit_read_local(uint8_t dest, uint8_t slot) {
    if (boxed_slots_.count(slot)) {          // boxed → cell: dest = slot[0]
        uint8_t z = alloc_temp();
        emit_load_const(z, Value((int64_t)0), cur_line_);
        emit(OpCode::ARRAY_GET, dest, slot, z);
        free_temp(z);
    } else {
        emit(OpCode::MOVE, dest, slot);
    }
}
void FunctionCompiler::emit_write_local(uint8_t slot, uint8_t src) {
    if (boxed_slots_.count(slot)) {          // boxed → cell: slot[0] = src
        uint8_t z = alloc_temp();
        emit_load_const(z, Value((int64_t)0), cur_line_);
        emit(OpCode::ARRAY_SET, slot, z, src);
        free_temp(z);
    } else {
        emit(OpCode::MOVE, slot, src);
    }
}
// Closure gövdesinde bir capture okunuyor. is_cell ise capture bir CELL (boxed
// local referansı) → LOAD_CAPTURE ile cell'i al, sonra [0] deref et (by-ref).
void FunctionCompiler::emit_read_capture(uint8_t dest, uint8_t cap_index) {
    if (cap_index < captures_.size() && captures_[cap_index].is_cell) {
        uint8_t c = alloc_temp();
        emit(OpCode::LOAD_CAPTURE, c, cap_index);
        uint8_t z = alloc_temp();
        emit_load_const(z, Value((int64_t)0), cur_line_);
        emit(OpCode::ARRAY_GET, dest, c, z);
        free_temp(z); free_temp(c);
    } else {
        emit(OpCode::LOAD_CAPTURE, dest, cap_index);
    }
}
// Bir VarLoc boxed cell mi: LOCAL ise boxed_slots_'ta mı; CAPTURE ise is_cell mi.
bool FunctionCompiler::is_cell_var(const VarLoc& loc) const {
    if (loc.kind == VarKind::LOCAL)   return boxed_slots_.count(loc.index) > 0;
    if (loc.kind == VarKind::CAPTURE) return loc.index < captures_.size() && captures_[loc.index].is_cell;
    return false;
}

// ── compile — entry point ─────────────────────────────────────────────────────

std::shared_ptr<FunctionProto> FunctionCompiler::compile(const BlockStatement& body,
        const std::vector<std::unique_ptr<Expression>>* defaults) {
    // ── Adım 2a: escape-analiz keşif-geçişi ───────────────────────────────────
    // Ayrı throwaway FC ile gövdeyi derle (state reset yok); hangi local'lerin
    // closure'lar tarafından yakalandığını topla. ÜRETİLEN KOD ATILIR — yalnız
    // boxed_names_ okunur. no_discovery_ sonsuz özyinelemeyi önler.
    // ŞU AN (2a) boxed_slots_ codegen'de KULLANILMAZ → davranış-değişmez.
    if (!no_discovery_) {
        FunctionCompiler disc(proto_.name, proto_.params, proto_.variadic, parent_);
        disc.captures_     = captures_;   // use() capture'larını miras al
        disc.no_discovery_ = true;
        disc.compile(body, defaults);     // derle (atılır) → disc.escaping_names_ dolar
        boxed_names_ = std::move(disc.escaping_names_);
        if (std::getenv("LOOK_DEBUG_BOXED") && !boxed_names_.empty()) {
            std::string s; for (auto& n : boxed_names_) s += n + " ";
            fprintf(stderr, "[BOXED] %s: %s\n", proto_.name.c_str(), s.c_str());
        }
    }

    // ── Prologue: varsayılan parametreler ────────────────────────────────────
    // Param i sağlanmadıysa (çağrıdaki argc <= i) varsayılanı doldur. Param'lar
    // ilk yerel register'ları (0..arity-1) tutar. Varsayılan ifade önceki
    // param'ları görebilir (soldan sağa). Interpreter ile aynı arity semantiği.
    if (defaults) {
        for (size_t i = 0; i < defaults->size() && i < proto_.arity; ++i) {
            if (!(*defaults)[i]) continue;
            uint8_t ac = alloc_temp();
            emit(OpCode::LOAD_ARGC, ac);
            uint8_t lim = alloc_temp();
            emit_load_const(lim, Value((int64_t)i), 0);
            uint8_t cond = alloc_temp();
            emit(OpCode::LTE, cond, ac, lim);         // argc <= i  → param i sağlanmadı
            free_temp(ac); free_temp(lim);
            int skip = emit_jump(OpCode::JUMP_IF_FALSE, cond);  // sağlandıysa varsayılanı atla
            free_temp(cond);
            uint8_t dr = compile_expr(*(*defaults)[i]); // varsayılan değeri
            emit(OpCode::MOVE, (uint8_t)i, dr);          // param i = varsayılan (reg i)
            free_temp(dr);
            patch_jump(skip, current_ip());
        }
    }

    // 58 simetri: closure tarafından yakalanan PARAMETRELER cell'e kutulanır.
    // Param'lar constructor'da declare_local'ı ATLAYIP slot alır (satır 50-56) →
    // boxed_slots_'a hiç girmezler; catch değişkeniyle aynı sınıf. Cell olmadan
    // capture snapshot okur → closure kurulduktan SONRA mutasyon ayrışır (tw by-ref
    // 105, vm snapshot 5). Defaults doldurulduktan SONRA kutula: defaults MOVE'u ham
    // slot bekler; burada slot nihai param değerini (verilen ya da varsayılan) tutar.
    for (int i = 0; i < proto_.arity; ++i) {
        if (!boxed_names_.count(proto_.params[i])) continue;
        boxed_slots_.insert((uint8_t)i);
        uint8_t tmp = alloc_temp();
        emit(OpCode::MOVE,       tmp, (uint8_t)i);   // gelen param değeri
        emit(OpCode::NEW_ARRAY,  (uint8_t)i, 1);     // slot = []
        emit(OpCode::ARRAY_PUSH, (uint8_t)i, tmp);   // slot = [value] (cell)
        free_temp(tmp);
    }

    compile_block(body);

    // Implicit return null
    emit(OpCode::RETURN_NULL);

    proto_.reg_count = regs_->max_used();
    // 58: capture'ların cell olup olmadığını proto'ya yaz → parallel() runtime'da
    // hangi capture'ı deep-clone edeceğini bilir (thread-safety).
    for (auto& c : captures_) proto_.capture_is_cell.push_back(c.is_cell ? 1 : 0);
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
    // Satır takibi: bundan sonra emit edilen opcode'lar bu statement'ın satırına yazılır.
    // proto_.lines ZATEN vardı ama emit() hep 0 basıyordu ("sonradan eklenecek") → VM
    // hataları satır/konum veremiyordu (tree-walk "File/Line" veriyor, VM sadece mesaj).
    if (stmt.loc.line > 0) cur_line_ = stmt.loc.line;
    if (auto* s = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        // Atama statement olarak kullanılıyorsa sonuç atılır → değeri geri-okuyan
        // MOVE'u atla. Aksi halde compile_expr(Assignment) her seferinde slot'u temp'e
        // kopyalar; `$s .= x` gibi akümülatörde bu O(n²) yaratır (kopya = tüm string).
        if (auto* a = dynamic_cast<const AssignmentExpression*>(s->expression.get())) {
            compile_assign_expr(*a);
        } else {
            // Sonuç kullanılmıyor — geçici register al, hemen bırak
            uint8_t r = compile_expr(*s->expression);
            free_temp(r);
        }
    }
    // print/write — interpreter semantiği (interpreter.cpp: build_output + PrintStatement):
    //   build_output: argümanlar " " ile AYRILIR;  print: sonuna "\n" EKLER;  write: eklemez.
    // Eskiden burada her argüman için tek tek builtin çağrılıyordu → ayraç da newline de
    // yoktu: print($a,$b) interpreter'da "a b\n", VM'de "ab" üretiyordu (hem CLI-VM hem
    // web-VM). Differential kaçırmıştı: gövde tek argümanla print ediyor ve $(...) zaten
    // trailing newline'ı yutuyor.
    else if (auto* s = dynamic_cast<const PrintStatement*>(&stmt)) {
        emit_output_args(s->expressions, /*newline=*/true);
    }
    else if (auto* s = dynamic_cast<const WriteStatement*>(&stmt)) {
        emit_output_args(s->expressions, /*newline=*/false);
    }
    else if (auto* s = dynamic_cast<const ReturnStatement*>(&stmt)) {
        compile_return(*s);
    }
    else if (auto* s = dynamic_cast<const ThrowStatement*>(&stmt)) {
        // throw <ifade> → değeri hesapla, THROW opcode (try_stack_ / LOAD_EXC yakalar)
        uint8_t r = compile_expr(*s->expression);
        emit(OpCode::THROW, r);
        free_temp(r);
    }
    else if (dynamic_cast<const BreakStatement*>(&stmt)) {
        // break en yakın breakable bağlama gider — switch veya döngü.
        if (loop_stack_.empty())
            throw LookCompileError("break outside a loop/switch");
        // döngü-içi try-finally'leri çalıştır (loop'un floor'una kadar), SONRA sıçra.
        emit_pending_finallys(loop_stack_.back().finally_floor);
        int p = emit_jump(OpCode::JUMP);
        loop_stack_.back().break_patches.push_back(p);
    }
    else if (dynamic_cast<const ContinueStatement*>(&stmt)) {
        // continue switch'i ATLAR — en yakın gerçek DÖNGÜYE gider (C semantiği).
        int idx = -1;
        for (int i = (int)loop_stack_.size() - 1; i >= 0; --i)
            if (!loop_stack_[i].is_switch) { idx = i; break; }
        if (idx < 0)
            throw LookCompileError("continue outside a loop");
        // hedef döngünün floor'una kadarki finally'leri çalıştır, SONRA sıçra.
        emit_pending_finallys(loop_stack_[idx].finally_floor);
        int target = loop_stack_[idx].continue_target;
        if (target >= 0) {
            // Hedef biliniyor (while/foreach)
            emit(OpCode::JUMP, 0, (uint8_t)(target >> 8), (uint8_t)(target & 0xFF));
        } else {
            int p = emit_jump(OpCode::JUMP);
            loop_stack_[idx].continue_patches.push_back(p);
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
    else if (auto* us = dynamic_cast<const UseStatement*>(&stmt)) {
        // 2c: keşif-geçişinde `use` İŞLENMEZ. Modül yükleme (bug 49) g_loaded dedup'ı
        // ile yan etkilidir; discovery pass onu tetiklerse REAL pass "zaten yüklü"
        // deyip modül kodunu atlar → route kalıcı interpreter'a düşer. Discovery
        // yalnız closure capture arar; modül (ayrı dosya) mevcut dosyanın top-level
        // loop-local'ini yakalamaz → `use`'u atlamak capture tespitini etkilemez.
        if (no_discovery_) return;
        // `use <ad>` iki farklı şeyi karşılar:
        //   1) stdlib modülü (string, jobs, template…) → builtin tablosunda ZATEN
        //      bağlı; bytecode'da yapılacak bir şey yok → NOP (eski davranış doğru).
        //   2) KULLANICI MODÜLÜ (~/.look/modules/<ad>/<ad>.lk) → dosya yüklenip
        //      fonksiyonları global olarak TANIMLANMALI.
        //
        // ESKİ HATA: her iki durumda da NOP vardı. Kullanıcı modülünün fonksiyonları
        // VM globals'ında hiç oluşmuyordu; interpreter onları yüklediği için çıktı
        // DOĞRU çıkıyor ama web'de route "Undefined variable: <fn>" verip KALICI
        // olarak interpreter'a düşüyordu. Ölçüldü (çok dosyalı proje, use model):
        //   [ERROR] VM BUG — route kalıcı interpreter'a düştü (YAVAŞ YOL):
        //           GET:/urun/{id} — Undefined variable: urun_getir
        // Yani dilin "use ile çok dosyalı proje" yolu web'de HIZINI kaybediyordu —
        // sonuç doğru olduğu için de sessiz kalıyordu (fallback maskeliyor).
        //
        // Çözüm: modül dosyasını burada parse edip statement'larını AYNI derleme
        // birimine kat. Böylece fonksiyonlar VM globals'ında BYTECODE_FN olur.
        // Ayrım basit: kullanıcı modülünün DOSYASI vardır, stdlib'inki yoktur.
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
                // Aynı modül iki kez use edilirse tekrar derleme (interpreter'ın
                // included_files_ mantığıyla aynı) — çift tanım ve şişme olmasın.
                static std::vector<std::string> g_loaded;
                static std::vector<std::shared_ptr<Program>> g_module_asts;  // AST ömrü
                bool already = false;
                for (auto& p : g_loaded) if (p == path) { already = true; break; }
                if (already) { emit(OpCode::NOP); return; }

                std::ifstream f(path);
                if (f) {
                    std::string src((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    try {
                        Lexer lx(src);
                        Parser p(lx.scan_tokens());
                        auto prog = std::shared_ptr<Program>(p.parse().release());
                        g_loaded.push_back(path);
                        g_module_asts.push_back(prog);
                        for (auto& sub : prog->statements) compile_stmt(*sub);
                        loaded = true;
                    } catch (...) {
                        // Modül derlenemedi → NOP; interpreter yolu yine çalışır
                        // (fallback), sessiz yanlış davranış üretmeyiz.
                    }
                }
            }
        }
        if (!loaded) emit(OpCode::NOP);
    }
    else {
        throw LookCompileError("Unknown statement type: " + std::string(typeid(stmt).name()));
    }
}

// ── compile_return ────────────────────────────────────────────────────────────

// try/catch içindeki return, RETURN'den ÖNCE bekleyen finally'leri içten-dışa çalıştırmalı.
// pending_finally_ geçici temizlenir ki finally-içi return kendini tekrar tetiklemesin.
void FunctionCompiler::emit_pending_finallys(size_t floor) {
    if (pending_finally_.size() <= floor) return;
    // [floor..end) suffix'ini içten-dışa çalıştır; geçici çıkar ki finally-içi return/break
    // kendini tekrar tetiklemesin, sonra geri koy.
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
    uint8_t r = compile_expr(*s.expression);
    // r TAHSİSLİ kalır → emit_pending_finallys'ın temp'leri onu ezmez (temp'ler üstte).
    emit_pending_finallys();
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

    loop_stack_.push_back({.continue_target = loop_start, .finally_floor = pending_finally_.size()});
    ++loop_depth_;                 // 2c: döngü-body → top-level loop-local cell olabilir
    compile_block(*s.body);
    --loop_depth_;
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

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

    loop_stack_.push_back({.continue_target = -1, .finally_floor = pending_finally_.size()}); // continue hedefi post sonrası
    ++loop_depth_;                 // 2c: döngü-body → top-level loop-local cell olabilir
    compile_block(*s.body);
    --loop_depth_;
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

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

    // 58 simetri: value/key var closure tarafından yakalanıyorsa (boxed) ayrı cell-slot
    // al; değilse sabit r_val/r_key'e map et (mevcut hızlı yol). Manuel kayıt declare_local'ı
    // atladığından foreach var'ı cell'i kaçırırdı → capture-sonrası mutasyon ayrışırdı
    // (tw by-ref 101, vm snapshot 1). Boxed cell FOR_STEP sonrası her iterasyon TAZE tahsis
    // edilir (per-iter, catch/param aynası).
    bool val_boxed = !s.value_var.empty() && boxed_names_.count(s.value_var) > 0;
    bool key_boxed = !s.key_var.empty()   && boxed_names_.count(s.key_var)   > 0;
    uint8_t val_cell = 0, key_cell = 0;
    if (val_boxed)                 val_cell = declare_local(s.value_var, 0);
    else if (!s.value_var.empty()) locals_.push_back({s.value_var, r_val, scope_depth_});
    if (key_boxed)                 key_cell = declare_local(s.key_var, 0);
    else if (!s.key_var.empty())   locals_.push_back({s.key_var,   r_key, scope_depth_});

    int loop_start = current_ip();
    // FOR_STEP: a=r_iter, b=exit_hi, c=exit_lo (sonradan patch)
    int step_ip = emit(OpCode::FOR_STEP, r_iter, 0, 0);
    // per-iter cell: FOR_STEP değeri r_val/r_key'e yazdıktan sonra taze cell'e kutula
    if (val_boxed) { emit(OpCode::NEW_ARRAY, val_cell, 1); emit(OpCode::ARRAY_PUSH, val_cell, r_val); }
    if (key_boxed) { emit(OpCode::NEW_ARRAY, key_cell, 1); emit(OpCode::ARRAY_PUSH, key_cell, r_key); }

    loop_stack_.push_back({.continue_target = loop_start, .finally_floor = pending_finally_.size()});
    ++loop_depth_;                 // 2c: döngü-body → top-level loop-local cell olabilir
    compile_block(*s.body);
    --loop_depth_;
    auto ctx = loop_stack_.back();
    loop_stack_.pop_back();

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

// Builtin indeksi artık 16-BİT: düşük 8 bit CALL_BUILTIN.b'de, yüksek 8 bit takip eden
// NOP hint'inin b alanında (bit-uyumlu: idx<=255 için NOP.b=0 = eski kodlama).
// Tarih: alan 8-bit'ken 255 üstü SESSİZCE kırpılıp YANLIŞ builtin çağrılıyordu
// (ampirik: liste 285'e çıkınca cache::size index 256 → 0 → print'i çağırdı, null döndü).
// Bu guard artık yalnız 16-bit tavanını (65535) korur — pratikte ulaşılmaz, ama sessiz
// kırpılma sınıfı bir daha ASLA sessiz olmasın diye duruyor.
void FunctionCompiler::check_builtin_index(int bidx, const std::string& name) {
    if (bidx > 65535)
        throw LookCompileError("builtin index exceeded 16-bit ceiling: '" + name + "' index=" +
                               std::to_string(bidx));
}

void FunctionCompiler::emit_output_args(const std::vector<std::unique_ptr<Expression>>& exprs,
                                        bool newline) {
    // builtin 0 (print) ve 1 (write) ikisi de argümanı HAM yazar (newline/ayraç eklemez)
    // → ayraç ve newline burada, interpreter'la aynı kuralla üretilir.
    auto emit_literal = [&](const char* text) {
        uint8_t r = alloc_temp();
        emit_load_const(r, Value(std::string(text)), 0);
        emit(OpCode::CALL_BUILTIN, r, 1 /*write*/, r);
        free_temp(r);
    };
    for (size_t i = 0; i < exprs.size(); ++i) {
        if (i > 0) emit_literal(" ");            // build_output: argümanlar arası " "
        uint8_t r = compile_expr(*exprs[i]);
        emit(OpCode::CALL_BUILTIN, r, 1 /*write*/, r);  // sonuç atılır
        free_temp(r);
    }
    if (newline) emit_literal("\n");             // PrintStatement: sonda "\n"
}

void FunctionCompiler::compile_try(const TryCatchStatement& s) {
    // finally'yi return-yolu için "bekleyen" yap: try+catch içindeki return önce onu çalıştırır.
    // Normal yol için ayrıca inline emit edilir (aşağıda). Yalnız try+catch derlenirken aktif.
    if (s.finally_block) pending_finally_.push_back(s.finally_block.get());

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
            if (boxed_slots_.count(e_reg)) {
                // 58 simetri: catch değişkeni bir closure tarafından yakalanıyorsa
                // (boxed) → cell tahsis et ve exception'ı cell[0]'a koy. Aksi halde
                // LOAD_EXC ham değeri slot'a yazar, boxed_slots slot'u cell sanır,
                // capture ARRAY_GET[0]'da null okurdu (atama-bildirimi 961-966 aynası).
                uint8_t tmp = alloc_temp();
                emit(OpCode::LOAD_EXC, tmp);
                emit(OpCode::NEW_ARRAY, e_reg, 1);
                emit(OpCode::ARRAY_PUSH, e_reg, tmp);
                free_temp(tmp);
            } else {
                emit(OpCode::LOAD_EXC, e_reg);
            }
        }
        compile_block(*s.catch_block);
        pop_scope();
    }

    // return-yolu "bekleyen" finally'yi burada bırak — try+catch derlendi (return-in-catch onu
    // kullandı). Bundan SONRAki finally emission'ları return için değil, normal/throw yol içindir.
    if (s.finally_block && !pending_finally_.empty() && pending_finally_.back() == s.finally_block.get())
        pending_finally_.pop_back();

    if (s.catch_block) {
        // catch YAKALADI → normal akış: finally (paylaşımlı) çalışır, re-raise YOK.
        int after_catch = current_ip();
        patch_jump(jump_end, after_catch);
        if (s.finally_block) compile_block(*s.finally_block);
    } else {
        // BUG #2 FIX: catch YOK. Eskiden throw catch_ip'e atlayıp inline finally'yi çalıştırıp
        // devam ediyordu → exception SESSİZCE YUTULUYORDU. Şimdi: throw-yolu (catch_ip'te)
        // exception'ı LOAD_EXC ile yakalar, finally çalıştırır, THROW ile YENİDEN fırlatır
        // (dıştaki try'a/C++'a propagate). Normal-yol AYRI finally kopyası çalıştırıp devam eder.
        uint8_t exc = alloc_temp();
        emit(OpCode::LOAD_EXC, exc);                        // orijinal exception (finally'den önce)
        if (s.finally_block) compile_block(*s.finally_block);  // throw-yolu finally
        emit(OpCode::THROW, exc);                           // re-raise
        free_temp(exc);
        int after = current_ip();
        patch_jump(jump_end, after);                        // normal-yol buraya iner
        if (s.finally_block) compile_block(*s.finally_block);  // normal-yol finally (ayrı kopya)
    }
}

// ── compile_func_decl ─────────────────────────────────────────────────────────

void FunctionCompiler::compile_func_decl(const FunctionDeclaration& s) {
    // Builtin gölgeleme = hata (interpreter ile parity — orada da tanım anında fırlatılır).
    // Compile hatası CLI'da tree-walk'a düşer, o da AYNI hatayı fırlatır → tutarlı.
    if (is_reserved_builtin(s.name))
        throw std::runtime_error("'" + s.name + "' is a builtin — cannot be redefined");
    // İç fonksiyon derle
    FunctionCompiler inner(s.name, s.parameters, s.is_variadic, this);
    auto proto = inner.compile(*s.body, &s.defaults);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    // Fonksiyon adı global scope'a kaydedilir
    uint16_t name_idx = add_const(Value(s.name));
    uint8_t  r        = alloc_temp();

    // İç fonksiyon dıştaki local/capture'ları yakalıyorsa (implicit capture —
    // inner.compile sırasında toplandı) compile_closure ile AYNI capture kurulumunu
    // yap. Eskiden bu blok YOKTU → proto gövdesindeki LOAD_CAPTURE'lar runtime'da
    // "Capture index out of range" ile ÇÖKÜYORDU (named nested fn capture, tree-walk çalışırken
    // VM crash). Capture yükleme MAKE_CLOSURE'dan hemen önce; hint'ler hemen sonra.
    std::vector<uint8_t> cap_regs;
    for (auto& cap : inner.captures_) {
        auto loc = resolve_var(cap.name);
        uint8_t cr = alloc_temp();
        if      (loc.kind == VarKind::LOCAL)   { escaping_names_.insert(cap.name); emit(OpCode::MOVE, cr, loc.index); }
        else if (loc.kind == VarKind::CAPTURE) emit(OpCode::LOAD_CAPTURE, cr, loc.index);
        else { uint16_t ni = add_const(Value(cap.name)); emit(OpCode::LOAD_GLOBAL, cr, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF)); }
        cap_regs.push_back(cr);
    }
    // MAKE_CLOSURE: a=r, b=fn_idx (nested index)
    emit(OpCode::MAKE_CLOSURE, r, (uint8_t)fn_idx);
    for (uint8_t cr : cap_regs) { emit(OpCode::LOAD_CAPTURE, 0, cr); free_temp(cr); }
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

    // switch bağlamı: case içindeki `break` switch sonuna atlar (dıştaki döngüye
    // DEĞİL). is_switch=true → continue bu bağlamı atlar, dıştaki döngüye gider.
    loop_stack_.push_back({.is_switch = true, .finally_floor = pending_finally_.size()});

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

    auto sctx = loop_stack_.back();
    loop_stack_.pop_back();

    int after = current_ip();

    // default yoksa no_match_jump → after
    bool has_default = false;
    for (auto& e : entries) if (e.is_default) { has_default = true; break; }
    if (!has_default) patch_jump(no_match_jump, after);

    for (int p : end_jumps)          patch_jump(p, after);
    for (int p : sctx.break_patches) patch_jump(p, after);  // break → switch sonu
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
    // Plain `$x = v` yazma target'ı (auto-capture YOK → dış değişkene atama yeni
    // local yaratır). `$arr[i]=v` ise $arr container'ı OKUNUP yerinde mutasyona
    // uğrar → auto-capture gerekir (referans tipi paylaşılır).
    auto loc = resolve_var(e.name, /*for_write=*/(e.index == nullptr));

    if (e.index) {
        // $arr[i] = val  ·  zincirli $l.s.x = v / $arr[0].x = v (object ifadeden)
        uint8_t arr;
        if (e.object) {
            // Zincirli: container ifadeyi derle — ARRAY/assoc referans olduğu için
            // ARRAY_SET yerinde mutasyon yapar, orijinali etkiler.
            arr = compile_expr(*e.object);
        } else {
            arr = alloc_temp();
            if      (loc.kind == VarKind::LOCAL)   emit_read_local(arr, loc.index);
            else if (loc.kind == VarKind::CAPTURE) emit_read_capture(arr, loc.index);
            else {
                uint16_t ni = add_const(Value(e.name));
                emit(OpCode::LOAD_GLOBAL, arr, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
            }
        }
        uint8_t idx = compile_expr(*e.index);
        uint8_t val = compile_expr(*e.value);
        // Compound (+= -= …): mevcut arr[idx]'i oku, birleştir, sonra yaz.
        if (e.op != "=") {
            static const std::unordered_map<std::string, OpCode> COMPOUND = {
                {"+=", OpCode::ADD},  {"-=", OpCode::SUB},  {"*=", OpCode::MUL},
                {"/=", OpCode::DIV},  {"%=", OpCode::MOD},  {".=", OpCode::CONCAT},
                {"&=", OpCode::BAND}, {"|=", OpCode::BOR},  {"^=", OpCode::BXOR},
            };
            auto it = COMPOUND.find(e.op);
            if (it == COMPOUND.end()) throw LookCompileError("Unknown compound op: " + e.op);
            uint8_t cur = alloc_temp();
            emit(OpCode::ARRAY_GET, cur, arr, idx);
            uint8_t res = alloc_temp();
            emit(it->second, res, cur, val);
            free_temp(cur); free_temp(val);
            val = res;
        }
        emit(OpCode::ARRAY_SET, arr, idx, val);
        free_temp(val); free_temp(idx); free_temp(arr);
        return;
    }

    uint8_t val = compile_expr(*e.value);

    if (loc.kind == VarKind::LOCAL) {
        // B7: `$s .= x` → yerinde CONCAT (dst==b==slot). VM append_in_place ile
        // amortize O(1); ayrıca cur/tmp MOVE'ları (iki O(n) kopya) elenir.
        if (e.op == ".=") {
            if (boxed_slots_.count(loc.index)) {
                // boxed: yerinde CONCAT yapılamaz (slot cell tutar) → cell[0] oku,
                // birleştir, cell[0]'a yaz.
                uint8_t cur = alloc_temp();
                emit_read_local(cur, loc.index);
                uint8_t res = alloc_temp();
                emit(OpCode::CONCAT, res, cur, val);
                emit_write_local(loc.index, res);
                free_temp(res); free_temp(cur); free_temp(val);
            } else {
                emit(OpCode::CONCAT, loc.index, loc.index, val);
                free_temp(val);
            }
        }
        // Compound assign için mevcut değeri oku
        else if (e.op != "=") {
            uint8_t cur = alloc_temp();
            emit_read_local(cur, loc.index);
            uint8_t tmp = alloc_temp();
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
        // LOOK by-value capture — capture değiştirilemiyor (felsefe)
        throw LookCompileError("Captured variable cannot be reassigned: $" + e.name);
    } else if (parent_ != nullptr ||
               (loop_depth_ > 0 && !outer_globals_.count(e.name) &&
                (no_discovery_ || boxed_names_.count(e.name)))) {
        // Fonksiyon içi VEYA (2c) top-level DÖNGÜ-BODY'de tanımlı + closure-yakalanan
        // top-level var → LOCAL (cell). Dar koşul: yalnız döngü-body'de tanımlı VE
        // yakalanan (boxed_names_). Discovery'de (no_discovery_) tüm döngü-body
        // top-level var'lar geçici local olur ki capture tespit edilsin; real'de
        // yalnız yakalananlar cell olur, gerisi STORE_GLOBAL kalır (route güvenli:
        // setup var'ları döngü-DIŞI → koşul tetiklenmez → global kalır).
        // Fonksiyon içi + isim local/capture değil → yeni FONKSIYON-LOCAL yarat.
        // Scope izolasyonu: fonksiyon dıştaki (global) değişkeni implicit ezemez
        // (interpreter ile aynı; VM'de eskiden STORE_GLOBAL ile sızıyordu — route
        // gövdesi local'leri global olduğundan eş zamanlı isteklerde de race'liydi).
        // Okuma hâlâ global'e düşer; val (RHS) yukarıda compile edildiği için
        // aynı-isim okuması doğru şekilde global'i gördü.
        uint8_t result = val;
        if (e.op != "=") {
            // compound: mevcut değeri GLOBAL'den oku (isim henüz local değil)
            uint16_t ni = add_const(Value(e.name));
            uint8_t cur = alloc_temp();
            emit(OpCode::LOAD_GLOBAL, cur, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
            uint8_t tmp = alloc_temp();
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
        uint8_t slot = declare_local(e.name, 0);
        if (boxed_slots_.count(slot)) {
            // 58: boxed local → cell = [result] (1-elemanlı array). Bu kod DÖNGÜ
            // gövdesindeyse her iterasyon çalışır → per-iterasyon TAZE cell (bedava).
            emit(OpCode::NEW_ARRAY, slot, 1);
            emit(OpCode::ARRAY_PUSH, slot, result);
        } else {
            emit(OpCode::MOVE, slot, result);
        }
        free_temp(result);
    } else {
        // Top-level (script global) → STORE_GLOBAL. Route/setup global'leri, app::
        // servisleri ve module referansları burada yaşar; davranış korunur.
        // 2c: döngü-DIŞI (loop_depth_==0) tanımlı top-level var → "outer global".
        // Döngü içindeki reassignment'ı cell YAPMAMALI (C2 paritesi — tree-walk
        // tek binding tutar). outer_globals_ bu ayrımı taşır.
        if (loop_depth_ == 0) outer_globals_.insert(e.name);
        uint16_t ni = add_const(Value(e.name));
        // BUG FIX: bu dal e.op'u YOK SAYIYORDU → top-level "$t += $i" sadece
        // "$t = $i" olarak derleniyordu (sol operand atılıyor). Diğer üç dal
        // (index, local, fonksiyon-içi) compound'u doğru yapıyordu; yalnız GLOBAL
        // yolu bozuktu → interpreter DOĞRU, VM SESSİZCE YANLIŞ (motor ayrışması).
        // Örn. top-level: $t=0; $t+=5 → VM'de 5 yerine... 5 (doğru sanılır) ama
        // $t=1; $t+=2 → 2 (3 değil); $s="x"; $s.="y" → "y" ("xy" değil).
        if (e.op != "=") {
            uint8_t cur = alloc_temp();
            emit(OpCode::LOAD_GLOBAL, cur, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
            uint8_t tmp = alloc_temp();
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
            // boxed (cell) local için doğrudan slot DÖNDÜRÜLEMEZ — slot cell'i tutar,
            // caller değeri bekler. emit_read_local cell[0] deref eder.
            if (dest == 255 && !boxed_slots_.count(loc.index)) return loc.index;
            uint8_t r = ensure_dest();
            emit_read_local(r, loc.index);
            return r;
        }
        uint8_t r = ensure_dest();
        if (loc.kind == VarKind::CAPTURE) {
            emit_read_capture(r, loc.index);
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
        if (loc.kind == VarKind::LOCAL) emit_read_local(r, loc.index);
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
        // ++/-- — VM'de eskiden desteklenmiyordu (compile hatası → interpreter'a
        // görünmez sapma). Interpreter ile aynı semantik: prefix yeni değeri,
        // postfix eski değeri döndürür; değişkene geri yazılır.
        if (e->op == "++" || e->op == "--") {
            auto* var = dynamic_cast<const Variable*>(e->right.get());
            if (!var) throw LookCompileError("++/-- requires a variable");
            OpCode delta_op = (e->op == "++") ? OpCode::ADD : OpCode::SUB;
            uint8_t one = alloc_temp();
            emit_load_const(one, Value((int64_t)1), expr.loc.line);
            auto loc = resolve_var(var->name);
            if (loc.kind == VarKind::CAPTURE)
                throw LookCompileError("Captured variable cannot be reassigned: $" + var->name);

            uint8_t r = ensure_dest();
            if (loc.kind == VarKind::LOCAL) {
                emit_read_local(r, loc.index);              // eski değer (postfix için; cell-farkında)
                uint8_t nv = alloc_temp();
                emit(delta_op, nv, r, one);                 // r değeri tutar → boxed/unboxed ikisi de doğru
                emit_write_local(loc.index, nv);
                if (e->prefix) emit(OpCode::MOVE, r, nv);   // prefix → yeni değer
                free_temp(nv);
            } else {
                // GLOBAL
                uint16_t ni = add_const(Value(var->name));
                emit(OpCode::LOAD_GLOBAL, r, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
                uint8_t nv = alloc_temp();
                emit(delta_op, nv, r, one);
                emit(OpCode::STORE_GLOBAL, nv, (uint8_t)(ni >> 8), (uint8_t)(ni & 0xFF));
                if (e->prefix) emit(OpCode::MOVE, r, nv);
                free_temp(nv);
            }
            free_temp(one);
            return r;
        }
        uint8_t operand = compile_expr(*e->right);
        uint8_t r = ensure_dest();
        if (e->op == "-")  emit(OpCode::UNM, r, operand);
        else if (e->op == "!") emit(OpCode::NOT, r, operand);
        else if (e->op == "~") emit(OpCode::BNOT, r, operand);
        else throw LookCompileError("Unknown unary op: " + e->op);
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

    throw LookCompileError("Unknown expression type: " + std::string(typeid(expr).name()),
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
        throw LookCompileError("Unknown binary op: " + e.op, e.loc.line);

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
            check_builtin_index(bidx, full);
            emit(OpCode::CALL_BUILTIN, r, (uint8_t)(bidx & 0xFF), base);
            // NOP hint: a=argc, b=builtin indeksin YUKSEK 8 biti (16-bit indeks).
            // Bit-uyumlu: idx<=255 icin b=0 = eski kodlama. 256 duvari boyle asildi.
            emit(OpCode::NOP, argc, (uint8_t)(bidx >> 8));
            for (int k = 0; k < argc; ++k) regs_->free(base + k);
            return r;
        }
        // Unknown modül fonksiyonu → genel CALL yolu.
        // İŞARETLE: bu çağrı derlenir ama RUNTIME'da "Not callable" fırlatır
        // (LOAD_GLOBAL "mod::fn" → null → CALL). Web'de route interpreter'a düşüp
        // kurtulur; CLI-VM'de fallback YOK → script çökerdi. CLI bayrağı görüp
        // tree-walk'a düşer (bkz. CompiledProgram::uses_non_builtin_module_fn).
        mark_non_builtin_module_fn();
    }
    if (auto* var = dynamic_cast<const Variable*>(e.callee.get())) {
        if (var->name == "parallel") {
            if (e.arguments.size() != 1)
                throw LookCompileError("parallel() takes one argument", e.loc.line);
            uint8_t closure = compile_expr(*e.arguments[0]);
            emit(OpCode::PARALLEL_CALL, closure);
            free_temp(closure);
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "send") {
            if (e.arguments.size() != 2)
                throw LookCompileError("send() takes 2 arguments", e.loc.line);
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
                throw LookCompileError("receive() takes 1 argument", e.loc.line);
            uint8_t ch = compile_expr(*e.arguments[0]);
            uint8_t r  = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CHAN_RECV, r, ch);
            free_temp(ch);
            return r;
        }
        if (var->name == "close") {
            // Argüman-sayısı kontrolü YOKTU → close() argümansız çağrılınca e.arguments[0]
            // boş vektörde null deref → DERLEME-ZAMANI SEGFAULT (tree-walk graceful hata
            // veriyordu → divergence). send/receive kalıbıyla doğrula.
            if (e.arguments.size() != 1)
                throw LookCompileError("close() takes 1 argument", e.loc.line);
            uint8_t ch = compile_expr(*e.arguments[0]);
            emit(OpCode::CHAN_CLOSE, ch);
            free_temp(ch);
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::LOAD_NULL, r);
            return r;
        }
        if (var->name == "chan_size") {
            if (e.arguments.size() != 1)
                throw LookCompileError("chan_size() takes 1 argument", e.loc.line);
            uint8_t ch = compile_expr(*e.arguments[0]);
            uint8_t r  = (dest == 255) ? alloc_temp() : dest;
            emit(OpCode::CHAN_SIZE, r, ch);
            free_temp(ch);
            return r;
        }
        // Interpreter global builtin alias'ları → VM builtin karşılığı.
        // Bu isimler interpreter'da inline çözülüyordu ama VM builtin listesinde
        // yoktu → route çağırınca CALL (bytecode-fn) olarak derlenip runtime'da
        // "Not callable" fırlatıyor, route KALICI interpreter'a düşüyordu
        // (O(n²) string dahil tüm VM optimizasyonları o route'ta boşa gidiyordu).
        static const std::unordered_map<std::string, std::string> BUILTIN_ALIAS = {
            {"len", "count"}, {"intval", "int"}, {"floatval", "float"},
            {"strval", "string"}, {"boolval", "bool"},
            {"json", "json::encode"},
            // Web: bare header()/redirect() interpreter global'iydi ama VM'de fallback
            // ediyordu. response::header/redirect module fonksiyonları ZATEN var ve
            // per-request web_ctx_'e bağlanıyor (response::text gibi) + CRLF-sanitize
            // ediyor (bonus). Alias → route interpreter'a düşmez.
            {"header", "response::header"}, {"redirect", "response::redirect"},
        };
        std::string bname = var->name;
        if (auto ai = BUILTIN_ALIAS.find(bname); ai != BUILTIN_ALIAS.end())
            bname = ai->second;
        // Bilinen built-in?
        int bidx = builtin_index(bname);
        if (bidx >= 0) {
            // alloc_seq ile ardışık register bloğu al — VM base+k varsayımına uyar
            uint8_t argc = (uint8_t)e.arguments.size();
            uint8_t base = (argc > 0) ? regs_->alloc_seq(argc) : 0;
            for (int k = 0; k < argc; ++k) {
                uint8_t ev = compile_expr(*e.arguments[k], base + k);
                if (ev != base + k) emit(OpCode::MOVE, base + k, ev);
            }
            uint8_t r = (dest == 255) ? alloc_temp() : dest;
            check_builtin_index(bidx, bname);
            emit(OpCode::CALL_BUILTIN, r, (uint8_t)(bidx & 0xFF), base);
            // NOP hint: a=argc, b=builtin indeksin YUKSEK 8 biti (16-bit indeks).
            // Bit-uyumlu: idx<=255 icin b=0 = eski kodlama. 256 duvari boyle asildi.
            emit(OpCode::NOP, argc, (uint8_t)(bidx >> 8));
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
    // argc ek byte olarak NOP'a gömülür — VM okur.
    // Callee basit bir AD ise adın sabit indeksi de (ni+1, 16-bit) NOP'un b/c
    // alanlarına gömülür — VM hata mesajında adı SÖYLEYEBİLSİN diye.
    // ESKİ HATA: VM'in CALL noktasında yalnız register değeri vardı, ad yoktu:
    //   olmayan_fonksiyon(1)
    //   VM        -> "Not callable (BYTECODE_FN expected)"   (hangi ad?)
    //   tree-walk -> "Undefined variable: olmayan_fonksiyon"
    // LOAD_GLOBAL bu adda fırlatamıyor çünkü "mod::fn" isimleri orada ıskalamak
    // ZORUNDA (genel CALL yoluna düşsünler diye) — o yüzden çözüm adı buraya
    // taşımak. 0 = ad yok (CALL_BUILTIN'in 16-bit indeks kalıbıyla aynı yöntem).
    uint16_t cname = 0;
    if (auto* cv = dynamic_cast<const Variable*>(e.callee.get()))
        cname = (uint16_t)(add_const(Value(cv->name)) + 1);
    emit(OpCode::NOP, argc, (uint8_t)(cname >> 8), (uint8_t)(cname & 0xFF));
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

    auto proto = inner.compile(*e.body, &e.defaults);
    int fn_idx = (int)proto_.nested.size();
    proto_.nested.push_back(proto);

    uint8_t r = (dest == 255) ? alloc_temp() : dest;

    // Capture değerlerini MAKE_CLOSURE'dan ÖNCE yükle — VM MAKE_CLOSURE'dan hemen
    // sonra art arda LOAD_CAPTURE(0, cr) hint'lerini bekler; araya MOVE giremez.
    // inner.captures_ = explicit use() (0..k-1) + gövde derlenirken keşfedilen
    // otomatik capture'lar (k..). Hepsini index sırasında parent scope'tan yükle.
    std::vector<uint8_t> cap_regs;
    for (auto& cap : inner.captures_) {
        const std::string& cap_name = cap.name;
        auto loc = resolve_var(cap_name);
        uint8_t cr = alloc_temp();
        if      (loc.kind == VarKind::LOCAL) {
            // 58 Adım 2a: bir closure BU fonksiyonun local'ini yakalıyor → o local
            // "kaçıyor" (escape), cell'e taşınmalı. İsmi topla (keşif-geçişi okur).
            escaping_names_.insert(cap_name);
            emit(OpCode::MOVE, cr, loc.index);
        }
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

    // Parçalara böl: sabit string + interpolation dönüşümlü. Interpreter'ın
    // interpolate_string() semantiğiyle BİREBİR: tetikleyici {$ · {harf · {_
    // (template {# değil), iç içe brace'lere saygı, fragment TAM expression olarak
    // lex+parse+compile ({$a + $b}, {$arr[1]}, {count($x)}, {mod::f()}...), parse
    // hatasında {…} literal olarak kalır. Aksi halde VM sessizce "null" üretirdi.
    // Tetikleyici SADECE '{$' — '$'-önekli formlar iki motorda da kesin
    // interpolation'dır (undefined '$var' → her ikisinde null). Dolarsız
    // '{identifier}' BİLİNÇLİ dışarıda: interpreter onu eval-hatasında literal'e
    // düşürür (runtime karar), VM derleyip null üretirdi → '{renk}' gibi şablon
    // string'lerinde web davranışını değiştirir. Güvenli/tutarlı sınır: '{$...}'.
    auto is_interp_start = [](const std::string& s, size_t p) -> bool {
        return s[p] == '{' && p + 1 < s.size() && s[p + 1] == '$';
    };
    std::vector<uint8_t> parts;
    std::vector<std::unique_ptr<Program>> keepalive; // parse edilen AST'ler compile bitene dek yaşamalı
    size_t i = 0;
    while (i < raw.size()) {
        if (is_interp_start(raw, i)) {
            // Eşleşen '}' — iç içe brace'lere saygı
            size_t depth = 1, j = i + 1;
            while (j < raw.size() && depth > 0) {
                if (raw[j] == '{') depth++;
                else if (raw[j] == '}') { if (--depth == 0) break; }
                ++j;
            }
            if (depth != 0) {  // kapanmayan brace → '{' literal, ilerle
                uint8_t pr = alloc_temp();
                emit_load_const(pr, Value(std::string(1, raw[i])), line);
                parts.push_back(pr);
                ++i;
                continue;
            }
            std::string expr_src = raw.substr(i + 1, j - i - 1);
            const std::string literal_src = raw.substr(i, j - i + 1);  // "{$x}" — hata hâlinde
            uint8_t pr = 255;
            try {
                Lexer lex(expr_src + ";");
                Parser par(lex.scan_tokens());
                auto prog = par.parse();
                if (!prog->statements.empty()) {
                    if (auto* es = dynamic_cast<ExpressionStatement*>(prog->statements[0].get())) {
                        // Fragment RUNTIME try/catch ile sarılır — interpreter parity:
                        // interpolate_string() fragment'ı eval ederken hata olursa (ör.
                        // tanımsız değişken → strict LOAD_GLOBAL fırlatır) "{…}" literal
                        // olarak bırakır. Sarmazsak strict VM fırlatır, interpreter literal
                        // döner → sapma. Parse hataları zaten aşağıda derleme-zamanı ele
                        // alınıyor; bu sarmalayıcı EVAL hataları içindir.
                        pr = alloc_temp();
                        int try_push_ip = emit(OpCode::TRY_PUSH, 0, 0, 0);
                        // Doğal register'a derle (dest=255): CALL sonucu arg-register
                        // düzenine bağlı, sabit dest zorlamak bozar. Sonra temp'e MOVE
                        // + TO_STR — local slot dönerse üstüne yazmayız.
                        uint8_t src = compile_expr(*es->expression);
                        emit(OpCode::MOVE, pr, src);
                        emit(OpCode::TO_STR, pr, pr);
                        emit(OpCode::TRY_POP);
                        int jump_end = emit_jump(OpCode::JUMP);
                        patch_jump(try_push_ip, current_ip());          // catch girişi
                        emit_load_const(pr, Value(literal_src), line);  // hata → literal
                        patch_jump(jump_end, current_ip());
                        keepalive.push_back(std::move(prog));
                    }
                }
            } catch (...) { pr = 255; }
            if (pr == 255) {  // parse hatası → interpreter gibi {…} literal
                pr = alloc_temp();
                emit_load_const(pr, Value(literal_src), line);
            }
            parts.push_back(pr);
            i = j + 1;
        } else {
            // Literal — sonraki interpolation tetikleyicisine kadar
            std::string literal;
            while (i < raw.size() && !is_interp_start(raw, i)) literal += raw[i++];
            if (!literal.empty()) {
                uint8_t pr = alloc_temp();
                emit_load_const(pr, Value(literal), line);
                parts.push_back(pr);
            }
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
    // 2c: top-level de escape-analiz keşif-geçişinden geçmeli (compile() gibi) —
    // yoksa top-level döngü-body-captured var'lar cell olmaz (C ayrışması kapanmaz).
    if (!no_discovery_) {
        FunctionCompiler disc(proto_.name, proto_.params, proto_.variadic, parent_);
        disc.captures_     = captures_;
        disc.no_discovery_ = true;
        disc.compile_stmts(stmts);
        boxed_names_ = std::move(disc.escaping_names_);
        if (std::getenv("LOOK_DEBUG_BOXED") && !boxed_names_.empty()) {
            std::string s; for (auto& n : boxed_names_) s += n + " ";
            fprintf(stderr, "[BOXED] %s: %s\n", proto_.name.c_str(), s.c_str());
        }
    }
    push_scope();
    for (auto& s : stmts) compile_stmt(*s);
    pop_scope();
    emit(OpCode::RETURN_NULL);
    proto_.reg_count = regs_->max_used();
    for (auto& c : captures_) proto_.capture_is_cell.push_back(c.is_cell ? 1 : 0);
    return std::make_shared<FunctionProto>(std::move(proto_));
}

// ── Compiler::compile — public entry point ────────────────────────────────────

CompiledProgram Compiler::compile(const Program& program) {
    FunctionCompiler main_compiler("<main>", {}, false, nullptr);
    auto proto = main_compiler.compile_stmts(program.statements);

    CompiledProgram out;
    out.main_proto = proto;
    out.uses_non_builtin_module_fn = main_compiler.used_non_builtin_module_fn();
    return out;
}

} // namespace look

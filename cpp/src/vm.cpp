// LOOK Bytecode VM
//
// Dispatch döngüsü: switch(opcode) → GCC/Clang jump table.
// Value tipi interpreter.h ile aynı — tam uyumluluk.
// Struct convention: ARRAY + __struct__ sentinel (interpreter ile aynı).

#include "look/vm.h"
#include "look/bytecode.h"
#include "look/web.h"
#include "look/logger.h"

#include <sstream>
#include <cmath>
#include <cassert>
#include <thread>

namespace look {

// ── VM ctor ───────────────────────────────────────────────────────────────────

VM::VM(SharedState shared, std::ostream& output)
    : shared_(shared), output_(output)
{
    regs_.reserve(1024);
    call_stack_.reserve(64);
}

void VM::set_globals(std::unordered_map<std::string, Value> g) { globals_ = std::move(g); }
void VM::set_web_context(WebContext* ctx)                        { web_ctx_ = ctx; }
void VM::set_ws_connection(std::shared_ptr<WsConnection> ws)    { ws_conn_ = ws; }
void VM::set_sse_connection(std::shared_ptr<SseConnection> sse) { sse_conn_ = sse; }

// ── execute ───────────────────────────────────────────────────────────────────

void VM::execute(const CompiledProgram& prog) {
    auto cl = std::make_shared<Closure>(prog.main_proto);
    push_frame(cl.get(), prog.main_proto->reg_count, -1);
    run();
}

// ── call_closure ──────────────────────────────────────────────────────────────

Value VM::call_closure(const Closure& closure, std::vector<Value> args) {
    if ((int)call_stack_.size() >= MAX_CALL_DEPTH)
        throw LookVmError("Stack overflow (max " + std::to_string(MAX_CALL_DEPTH) + ")");

    auto* proto = closure.proto.get();
    int base = push_frame(&closure, proto->reg_count, -1);

    // Argümanları register'lara yaz
    int argc = (int)args.size();
    if (!proto->variadic) {
        for (int i = 0; i < proto->arity && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
    } else {
        int fixed = proto->arity - 1;
        for (int i = 0; i < fixed && i < argc; ++i)
            regs_[base + i] = std::move(args[i]);
        // Variadic: kalan args → ARRAY
        auto varr = std::make_shared<std::vector<Value>>();
        for (int i = fixed; i < argc; ++i) varr->push_back(std::move(args[i]));
        if (fixed >= 0 && proto->arity > 0)
            regs_[base + fixed] = Value(varr);
    }

    return run();
}

// ── push_frame ────────────────────────────────────────────────────────────────

int VM::push_frame(const Closure* cl, int reg_count, int ret_reg) {
    int base = (int)regs_.size();
    regs_.resize(base + reg_count);
    call_stack_.push_back({cl->proto.get(), cl, 0, base, ret_reg});
    return base;
}

// ── dispatch_routes ───────────────────────────────────────────────────────────

void VM::dispatch_routes(const std::string& method, const std::string& path) {
    if (!shared_.routes) return;
    for (auto& [pattern, closure] : *shared_.routes) {
        size_t colon = pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = pattern.substr(0, colon);
        std::string p = pattern.substr(colon + 1);
        if (m != method && m != "*") continue;
        std::vector<Value> params;
        if (route_match(p, path, params)) {
            call_closure(*closure, std::move(params));
            return;
        }
    }
    // 404 route ara ("404:/" veya "404:/..." pattern'i)
    for (auto& [pattern, closure] : *shared_.routes) {
        size_t colon = pattern.find(':');
        if (colon == std::string::npos) continue;
        std::string m = pattern.substr(0, colon);
        if (m == "404") {
            call_closure(*closure, {});
            return;
        }
    }
    if (web_ctx_) { web_ctx_->status_code = 404; web_ctx_->status_text = "Not Found"; }
    output_ << "404 Not Found";
}

bool VM::route_match(const std::string& pattern, const std::string& path,
                     std::vector<Value>& params) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream ss(s);
        std::string t;
        while (std::getline(ss, t, '/')) if (!t.empty()) v.push_back(t);
        return v;
    };
    auto pp = split(pattern), rp = split(path);
    if (pp.size() != rp.size()) return false;
    for (size_t i = 0; i < pp.size(); ++i) {
        if (pp[i].size() > 2 && pp[i].front() == '{' && pp[i].back() == '}')
            params.push_back(Value(rp[i]));
        else if (pp[i] != rp[i]) return false;
    }
    return true;
}

// ── Value helpers ─────────────────────────────────────────────────────────────

bool VM::val_truthy(const Value& v) { return v.is_truthy(); }

std::string VM::val_to_str(const Value& v) { return v.to_string(); }

Value VM::array_get(const Value& arr, const Value& key) {
    if (arr.type() != Value::ARRAY) return Value();
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, k1, v1, ...] — sentinel at position 0 (interpreter convention)
    if (!vec.empty() && vec[0].type() == Value::STRING && vec[0].as_string() == "__assoc__") {
        std::string k = key.to_string();
        // Also handle __struct__ tag: ["__assoc__", "__struct__", "Name", k, v, ...]
        size_t start = 1;
        if (vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].as_string() == "__struct__")
            start = 3;
        for (size_t i = start; i + 1 < vec.size(); i += 2) {
            if (vec[i].to_string() == k) return vec[i+1];
        }
        return Value();
    }
    // Numeric
    if (key.type() == Value::INT) {
        int idx = key.as_int();
        if (idx >= 0 && idx < (int)vec.size()) return vec[idx];
    } else if (key.type() == Value::STRING) {
        try { int idx = std::stoi(key.as_string());
              if (idx >= 0 && idx < (int)vec.size()) return vec[idx]; }
        catch(...) {}
    }
    return Value();
}

void VM::array_set(Value& arr, const Value& key, const Value& val) {
    if (arr.type() == Value::NONE) {
        arr = Value(std::make_shared<std::vector<Value>>());
    }
    if (arr.type() != Value::ARRAY) return;
    auto& vec = *arr.as_array();

    // Assoc: ["__assoc__", k0, v0, ...] — sentinel at position 0 (interpreter convention)
    bool is_assoc = !vec.empty() && vec[0].type() == Value::STRING && vec[0].as_string() == "__assoc__";
    if (is_assoc || key.type() == Value::STRING) {
        std::string k = key.to_string();
        // Find existing (skip sentinel at [0] and optional struct tags)
        size_t start = is_assoc ? 1 : 0;
        if (is_assoc && vec.size() > 2 && vec[1].type() == Value::STRING && vec[1].as_string() == "__struct__")
            start = 3;
        for (size_t i = start; i + 1 < vec.size(); i += 2) {
            if (vec[i].to_string() == k) { vec[i+1] = val; return; }
        }
        // New entry
        if (!is_assoc) {
            // Convert to assoc: insert sentinel at position 0
            vec.insert(vec.begin(), Value(std::string("__assoc__")));
        }
        // Append key, value at end
        vec.push_back(Value(k));
        vec.push_back(val);
        return;
    }
    // Numeric
    int idx = key.as_int();
    if (idx >= 0 && idx < (int)vec.size()) vec[idx] = val;
    else if (idx == (int)vec.size()) vec.push_back(val);
}

Value VM::get_field(const Value& obj, const std::string& field) {
    return array_get(obj, Value(field));
}

void VM::set_field(Value& obj, const std::string& field, const Value& val) {
    array_set(obj, Value(field), val);
}

// ── run — dispatch döngüsü ────────────────────────────────────────────────────

Value VM::run() {
call_dispatch:
    while (!call_stack_.empty()) {
        Frame& frame = call_stack_.back();
        const FunctionProto* proto = frame.proto;
        int base = frame.base;

        while (frame.ip < (int)proto->code.size()) {
            const Instruction& ins = proto->code[frame.ip++];

#define R(x) regs_.at((size_t)(base + (x)))
#define CONST(i) proto->constants[i]

            switch (ins.op) {

            // ── Load ──────────────────────────────────────────────────────────
            case OpCode::LOAD_CONST:
                R(ins.a) = CONST(ins.b);
                break;
            case OpCode::LOAD_CONST_W:
                R(ins.a) = CONST((uint16_t(ins.b)<<8)|ins.c);
                break;
            case OpCode::LOAD_NULL:  R(ins.a) = Value();        break;
            case OpCode::LOAD_TRUE:  R(ins.a) = Value(true);    break;
            case OpCode::LOAD_FALSE: R(ins.a) = Value(false);   break;
            case OpCode::LOAD_INT:   R(ins.a) = Value((int)(int8_t)ins.b); break;
            case OpCode::MOVE:       R(ins.a) = R(ins.b);       break;
            case OpCode::LOAD_VAR:   R(ins.a) = R(ins.b);       break;
            case OpCode::STORE_VAR:  R(ins.a) = R(ins.b);       break;

            case OpCode::LOAD_GLOBAL: {
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                auto it = globals_.find(CONST(ni).as_string());
                R(ins.a) = (it != globals_.end()) ? it->second : Value();
                break;
            }
            case OpCode::STORE_GLOBAL: {
                // a=src_reg, b=name_hi, c=name_lo (16-bit constant pool index)
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                const std::string& name = CONST(ni).as_string();
                globals_[name] = R(ins.a);
                break;
            }

            // ── Aritmetik — Value'nun operatörlerini kullan ───────────────────
            case OpCode::ADD:    R(ins.a) = R(ins.b) + R(ins.c);        break;
            case OpCode::SUB:    R(ins.a) = R(ins.b) - R(ins.c);        break;
            case OpCode::MUL:    R(ins.a) = R(ins.b) * R(ins.c);        break;
            case OpCode::DIV:    R(ins.a) = R(ins.b) / R(ins.c);        break;
            case OpCode::MOD:    R(ins.a) = R(ins.b) % R(ins.c);        break;
            case OpCode::POW:    R(ins.a) = R(ins.b).pow(R(ins.c));     break;
            case OpCode::UNM: {
                const Value& b = R(ins.b);
                R(ins.a) = (b.type()==Value::INT) ? Value(-b.as_int()) : Value(-b.as_float());
                break;
            }
            case OpCode::BAND:   R(ins.a) = R(ins.b).bitwise_and(R(ins.c)); break;
            case OpCode::BOR:    R(ins.a) = R(ins.b).bitwise_or(R(ins.c));  break;
            case OpCode::BXOR:   R(ins.a) = R(ins.b).bitwise_xor(R(ins.c)); break;
            case OpCode::BNOT:   R(ins.a) = R(ins.b).bitwise_not();         break;
            case OpCode::SHL:    R(ins.a) = R(ins.b).shift_left(R(ins.c));  break;
            case OpCode::SHR:    R(ins.a) = R(ins.b).shift_right(R(ins.c)); break;
            case OpCode::CONCAT: R(ins.a) = R(ins.b).concat(R(ins.c));      break;

            // ── Karşılaştırma ─────────────────────────────────────────────────
            case OpCode::EQ:  R(ins.a) = Value(R(ins.b) == R(ins.c)); break;
            case OpCode::NEQ: R(ins.a) = Value(!(R(ins.b) == R(ins.c))); break;
            case OpCode::LT:  R(ins.a) = Value(R(ins.b) <  R(ins.c)); break;
            case OpCode::GT:  R(ins.a) = Value(R(ins.b) >  R(ins.c)); break;
            case OpCode::LTE: R(ins.a) = Value(R(ins.b) <= R(ins.c)); break;
            case OpCode::GTE: R(ins.a) = Value(R(ins.b) >= R(ins.c)); break;
            case OpCode::CMP3: R(ins.a) = Value(R(ins.b).spaceship(R(ins.c))); break;
            case OpCode::NOT: R(ins.a) = Value(!R(ins.b).is_truthy()); break;
            case OpCode::COALESCE:
                R(ins.a) = (R(ins.b).type() != Value::NONE) ? R(ins.b) : R(ins.c);
                break;

            // ── Array ─────────────────────────────────────────────────────────
            case OpCode::NEW_ARRAY: {
                auto v = std::make_shared<std::vector<Value>>();
                v->reserve(ins.b);
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::NEW_ASSOC: {
                // interpreter convention: assoc sentinel at end
                auto v = std::make_shared<std::vector<Value>>();
                v->push_back(Value(std::string("__assoc__")));
                R(ins.a) = Value(v);
                break;
            }
            case OpCode::ARRAY_PUSH:
                if (R(ins.a).type() == Value::ARRAY)
                    R(ins.a).as_array()->push_back(R(ins.b));
                break;
            case OpCode::ARRAY_GET:
                R(ins.a) = array_get(R(ins.b), R(ins.c));
                break;
            case OpCode::ARRAY_SET:
                array_set(R(ins.a), R(ins.b), R(ins.c));
                break;
            case OpCode::ARRAY_LEN: {
                const Value& arr = R(ins.b);
                int len = 0;
                if (arr.type() == Value::ARRAY) {
                    auto& vec = *arr.as_array();
                    // Sentinel-first: ["__assoc__", k, v, ...] → (size-1)/2 pairs
                    if (!vec.empty() && vec[0].type()==Value::STRING && vec[0].as_string()=="__assoc__") {
                        size_t data_start = 1;
                        if (vec.size()>2 && vec[1].as_string()=="__struct__") data_start=3;
                        len = (int)(vec.size()-data_start)/2;
                    } else {
                        len = (int)vec.size();
                    }
                }
                R(ins.a) = Value(len);
                break;
            }

            // ── Struct — ARRAY + __struct__ sentinel (interpreter uyumlu) ─────
            case OpCode::NEW_STRUCT: {
                uint16_t ni = (uint16_t(ins.b)<<8)|ins.c;
                std::string sname = CONST(ni).as_string();
                auto v = std::make_shared<std::vector<Value>>();
                // interpreter convention: ["__assoc__","__struct__","StructName", fields...]
                v->push_back(Value(std::string("__assoc__")));
                v->push_back(Value(std::string("__struct__")));
                v->push_back(Value(sname));
                // Default field değerleri
                if (shared_.struct_defs) {
                    auto it = shared_.struct_defs->find(sname);
                    if (it != shared_.struct_defs->end()) {
                        for (auto& f : it->second) {
                            v->push_back(Value(f.name));
                            v->push_back(f.default_val);
                        }
                    }
                }
                R(ins.a) = Value(v);
                // NOP(field_count) + LOAD_CONST_W*N skip
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    int fc = proto->code[frame.ip].a;
                    frame.ip += 1 + fc;
                }
                break;
            }
            case OpCode::GET_FIELD: {
                uint8_t fi = ins.c;
                const std::string& fname = CONST(fi).as_string();
                R(ins.a) = get_field(R(ins.b), fname);
                break;
            }
            case OpCode::SET_FIELD: {
                uint8_t fi = ins.b;
                const std::string& fname = CONST(fi).as_string();
                set_field(R(ins.a), fname, R(ins.c));
                break;
            }

            // ── Jump ──────────────────────────────────────────────────────────
            case OpCode::JUMP:
                frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_FALSE:
                if (!R(ins.a).is_truthy()) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_TRUE:
                if ( R(ins.a).is_truthy()) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;
            case OpCode::JUMP_IF_NULL:
                if (R(ins.a).type()==Value::NONE) frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                break;

            // ── CALL ──────────────────────────────────────────────────────────
            case OpCode::CALL: {
                // a=ret_reg, b=fn_reg, c=args_base; sonraki NOP(argc)
                uint8_t argc = 0;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc = proto->code[frame.ip].a;
                    ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN)
                    throw LookVmError("Çağrılabilir değil (BYTECODE_FN bekleniyor)");
                auto cl = fn_val.as_bytecode_fn();
                int new_base = (int)regs_.size();
                regs_.resize(new_base + cl->proto->reg_count);
                for (int i = 0; i < argc; ++i)
                    regs_[new_base + i] = R(ins.c + i);
                int ret_abs = base + ins.a;
                call_stack_.push_back({cl->proto.get(), cl.get(), 0, new_base, ret_abs});
                goto call_dispatch; // callee frame'e geç
            }

            case OpCode::CALL_BUILTIN: {
                uint8_t argc2 = 1;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc2 = proto->code[frame.ip].a;
                    ++frame.ip;
                }
                if (!shared_.builtins || ins.b >= shared_.builtins->size())
                    throw LookVmError("Bilinmeyen built-in: " + std::to_string(ins.b));
                std::vector<Value> args;
                args.reserve(argc2);
                for (int i = 0; i < argc2; ++i) args.push_back(R(ins.c + i));
                R(ins.a) = (*shared_.builtins)[ins.b](args);
                break;
            }

            case OpCode::RETURN: {
                Value ret = R(ins.a);
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = std::move(ret);
                    goto call_dispatch; // caller frame'e dön
                } else {
                    return ret;
                }
            }
            case OpCode::RETURN_NULL: {
                int ret_reg = frame.ret_reg;
                regs_.resize(base);
                call_stack_.pop_back();
                if (!call_stack_.empty() && ret_reg >= 0) {
                    regs_[ret_reg] = Value();
                    goto call_dispatch; // caller frame'e dön
                } else {
                    return Value();
                }
            }
            case OpCode::TAIL_CALL: {
                // Aynı CALL mantığı — frame yeniden kullan (tail call opt)
                // Şimdilik normal CALL gibi davran — optimize edilmemiş
                uint8_t argc3 = 0;
                if (frame.ip < (int)proto->code.size()
                    && proto->code[frame.ip].op == OpCode::NOP) {
                    argc3 = proto->code[frame.ip].a; ++frame.ip;
                }
                const Value& fn_val = R(ins.b);
                if (fn_val.type() != Value::BYTECODE_FN)
                    throw LookVmError("TAIL_CALL: BYTECODE_FN bekleniyor");
                auto cl = fn_val.as_bytecode_fn();
                int new_base = (int)regs_.size();
                regs_.resize(new_base + cl->proto->reg_count);
                for (int i = 0; i < argc3; ++i) regs_[new_base+i] = R(ins.c+i);
                call_stack_.push_back({cl->proto.get(), cl.get(), 0, new_base, base+ins.a});
                goto call_dispatch; // callee frame'e geç
            }

            // ── Closure ───────────────────────────────────────────────────────
            case OpCode::MAKE_CLOSURE: {
                auto& nested_proto = proto->nested[ins.b];
                auto cl = std::make_shared<Closure>(nested_proto);
                // Capture hint'leri oku: LOAD_CAPTURE(0, cr) pattern
                while (frame.ip < (int)proto->code.size()) {
                    const auto& h = proto->code[frame.ip];
                    if (h.op != OpCode::LOAD_CAPTURE || h.a != 0) break;
                    cl->captures.push_back(R(h.b));
                    ++frame.ip;
                }
                R(ins.a) = Value(cl);
                break;
            }
            case OpCode::LOAD_CAPTURE: {
                if (!frame.closure || ins.b >= frame.closure->captures.size())
                    throw LookVmError("Capture index dışı: " + std::to_string(ins.b));
                R(ins.a) = frame.closure->captures[ins.b];
                break;
            }

            // ── Foreach ───────────────────────────────────────────────────────
            case OpCode::FOR_PREP:
                // r_iter = array copy, r_iter+1 = index 0
                R(ins.a)   = R(ins.b);
                R(ins.a+1) = Value(0);
                break;
            case OpCode::FOR_STEP: {
                const Value& arr = R(ins.a);
                int idx = R(ins.a+1).as_int();
                if (arr.type() != Value::ARRAY) { frame.ip=(uint16_t(ins.b)<<8)|ins.c; break; }
                auto& vec = *arr.as_array();
                // Sentinel-first: ["__assoc__", k0, v0, k1, v1, ...]
                bool is_assoc = !vec.empty() && vec[0].type()==Value::STRING
                                && vec[0].as_string()=="__assoc__";
                if (is_assoc) {
                    // Skip __assoc__ sentinel + optional __struct__ tag
                    int data_start = 1;
                    if (vec.size()>2 && vec[1].type()==Value::STRING && vec[1].as_string()=="__struct__")
                        data_start = 3;
                    int pi = data_start + idx * 2;
                    if (pi + 1 >= (int)vec.size()) {
                        frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                    } else {
                        R(ins.a+3) = vec[pi];     // key
                        R(ins.a+2) = vec[pi+1];   // val
                        R(ins.a+1) = Value(idx+1);
                    }
                } else {
                    if (idx >= (int)vec.size()) {
                        frame.ip = (uint16_t(ins.b)<<8)|ins.c;
                    } else {
                        R(ins.a+2) = vec[idx];    // val
                        R(ins.a+3) = Value(idx);  // key
                        R(ins.a+1) = Value(idx+1);
                    }
                }
                break;
            }

            // ── Try/Catch ─────────────────────────────────────────────────────
            case OpCode::TRY_PUSH: {
                int catch_ip = (uint16_t(ins.b)<<8)|ins.c;
                try_stack_.push_back({catch_ip, (int)call_stack_.size(), base});
                break;
            }
            case OpCode::TRY_POP:
                if (!try_stack_.empty()) try_stack_.pop_back();
                break;
            case OpCode::THROW: {
                current_exception_ = R(ins.a);
                if (try_stack_.empty())
                    throw LookVmError("Yakalanmamış: " + current_exception_.to_string());
                auto entry = try_stack_.back(); try_stack_.pop_back();
                while ((int)call_stack_.size() > entry.frame_depth) {
                    regs_.resize(call_stack_.back().base);
                    call_stack_.pop_back();
                }
                call_stack_.back().ip = entry.catch_ip;
                break;
            }
            case OpCode::LOAD_EXC:
                R(ins.a) = current_exception_;
                break;

            // ── LOOK'a özgü ───────────────────────────────────────────────────
            case OpCode::PARALLEL_CALL: {
                if (R(ins.a).type() != Value::BYTECODE_FN)
                    throw LookVmError("parallel(): BYTECODE_FN bekleniyor");
                auto cl_copy = R(ins.a).as_bytecode_fn();
                SharedState sh = shared_;
                std::unordered_map<std::string, Value> g_copy = globals_;
                // builtins ve routes local pointer'lara işaret eder — goroutine için deep copy
                std::vector<BuiltinFn> builtins_copy = sh.builtins ? *sh.builtins : std::vector<BuiltinFn>{};
                sh.builtins = nullptr;
                sh.routes   = nullptr; // goroutine dispatch_routes çağırmaz
                std::thread([cl_copy, sh, g_copy, builtins_copy = std::move(builtins_copy)]() mutable {
                    std::ostringstream sink;
                    VM tvm(sh, sink);
                    tvm.set_globals(std::move(g_copy));
                    tvm.set_builtins(&builtins_copy);
                    try { tvm.call_closure(*cl_copy, {}); }
                    catch (const std::exception& e) {
                        Logger::instance().log(LogLevel::LOG_ERROR, "VM",
                            std::string("parallel() error: ") + e.what());
                    }
                }).detach();
                break;
            }
            case OpCode::CHAN_SEND: {
                auto ch = R(ins.a).as_channel();
                if (ch) ch->send_val(R(ins.b));
                break;
            }
            case OpCode::CHAN_RECV: {
                auto ch = R(ins.b).as_channel();
                R(ins.a) = ch ? ch->recv_val() : Value();
                break;
            }
            case OpCode::CHAN_CLOSE: {
                auto ch = R(ins.a).as_channel();
                if (ch) { std::unique_lock<std::mutex> lk(ch->mtx); ch->closed = true; ch->not_empty.notify_all(); }
                break;
            }
            case OpCode::CHAN_SIZE: {
                auto ch = R(ins.b).as_channel();
                R(ins.a) = ch ? Value(ch->sz()) : Value(0);
                break;
            }

            // ── Cast / Type ───────────────────────────────────────────────────
            case OpCode::TO_INT:   R(ins.a) = Value(R(ins.b).to_int());   break;
            case OpCode::TO_FLOAT: R(ins.a) = Value(R(ins.b).to_float()); break;
            case OpCode::TO_STR:   R(ins.a) = Value(R(ins.b).to_string()); break;
            case OpCode::TO_BOOL:  R(ins.a) = Value(R(ins.b).is_truthy()); break;
            case OpCode::TYPE_OF: {
                static const char* tnames[] = {
                    "int","float","string","bool","function","array",
                    "channel","websocket","sse","function","null"
                };
                int ti = (int)R(ins.b).type();
                R(ins.a) = Value(std::string(ti < 11 ? tnames[ti] : "unknown"));
                break;
            }

            case OpCode::NOP:       break;
            case OpCode::BREAKPOINT:
                Logger::instance().log(LogLevel::LOG_DEBUG, "VM", "BREAKPOINT");
                break;

            default:
                throw LookVmError("Bilinmeyen opcode: " + std::to_string((int)ins.op));

#undef R
#undef CONST
            } // switch
        } // inner while

        // Implicit return null
        int ret_reg = frame.ret_reg;
        regs_.resize(base);
        call_stack_.pop_back();
        if (call_stack_.empty() || ret_reg < 0) return Value();
        regs_[ret_reg] = Value();

    } // outer while
    return Value();
}

} // namespace look

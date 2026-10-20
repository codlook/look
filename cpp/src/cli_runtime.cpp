// cli_runtime.cpp — komut satırı VM ortamı: yerleşik tablo + geri çağırma bağlamaları.
// `lk` (main.cpp) ve `lk test` (test_runner.cpp) AYNI tabloyu kullanır — testler programın
// gerçekte koştuğu ortamı sınar (LOOK 2 tek motor, adım 6).
#include "look/cli_runtime.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/capture_check.h"
#include "look/arg_check.h"
#include "look/interpreter.h"
#include "look/array_count.h"
#include "look/http_client.h"
#include "look/runtime_init.h"    // runtime_init (CA probe + SQLite init, süreç başı)
#include "look/web.h"
#include "look/parallel_runtime.h"
#include "look/ast.h"
#include "look/builtins.h"
#include "look/compiler.h"
#include "look/vm.h"
#include "look/use_modules.h"   // collect_use_modules — CLI ve web aynı `use` listesi
#include "look/timer.h"         // TimerManager — komut satırında timer:: VM closure'ı kurar
#include "look/setup_handoff.h" // vm_closure_runner — jobs::run işleyicilerini VM'de koşturur
#include <memory>
#include <unordered_map>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <functional>

namespace look {

// ── C9 (CLI-VM, opt-in) — CLI top-level script'i bytecode VM'de çalıştırmak için
// builtin tablosu. http_main req_builtins ile aynı semantik; çıktı `out`'a gider.
// Modül fn'leri interpreter'ın YÜKLÜ modüllerinden auto-wire olur (use gerektirir).
std::vector<look::BuiltinFn> build_cli_builtins(look::Interpreter& interp, std::ostream& out) {
    using look::Value;
    std::vector<look::BuiltinFn> b(look::builtin_names().size());
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    b[0] = [&out](std::vector<Value>& a) -> Value { for (auto& x : a) out << x.to_string(); return Value(); };
    b[1] = b[0];  // print/write
    b[2] = [](std::vector<Value>& a) -> Value {   // count/len — tek tanım (array_count.h)
        return Value(a.empty() ? 0 : look_count(a[0]));   // assoc: (size-1)/2 (sentinel atlanır)
    };
    b[5] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? std::string() : a[0].to_string()); };  // str
    b[6] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); return Value(a[0].to_int()); };  // int — to_int(): stoll(to_string(float)) bilimsel-gösterim bug'ı + int32 daralması kapandı (tree-walk parite)
    b[7] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0.0); return Value(a[0].to_float()); };  // float — to_float(): to_string() round-trip yok (tree-walk parite)
    b[8] = [](std::vector<Value>& a) -> Value {   // bool
        if (a.empty()) return Value(false);
        auto& v = a[0];
        if (v.type()==Value::BOOL) return v;
        if (v.type()==Value::INT)  return Value(v.as_int()!=0);
        if (v.type()==Value::FLOAT)return Value(v.as_float()!=0.0);
        if (v.type()==Value::STRING)return Value(!v.as_string().empty());
        if (v.type()==Value::NONE) return Value(false);
        return Value(true);
    };
    b[9] = b[5];  // string alias
    b[22] = [](std::vector<Value>&) -> Value { return Value(); };  // route — CLI'da dispatch yok
    b[BI("strlen")] = [](std::vector<Value>& a) -> Value { return Value(a.empty() ? 0 : (int)a[0].to_string().size()); };
    b[BI("abs")] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(0); if (a[0].type()==Value::FLOAT) return Value(std::abs(a[0].as_float())); return Value(std::abs(a[0].to_int())); };
    b[BI("max")] = [](std::vector<Value>& a) -> Value { if (a.size()<2) return a.empty()?Value():a[0]; return a[0]>=a[1]?a[0]:a[1]; };
    b[BI("min")] = [](std::vector<Value>& a) -> Value { if (a.size()<2) return a.empty()?Value():a[0]; return a[0]<=a[1]?a[0]:a[1]; };
    b[BI("sqrt")] = [](std::vector<Value>& a) -> Value { return Value(a.empty()?0.0:std::sqrt(a[0].to_float())); };
    b[BI("strtoupper")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::toupper((unsigned char)c); return Value(s); };
    b[BI("strtolower")] = [](std::vector<Value>& a) -> Value { std::string s=a.empty()?"":a[0].to_string(); for(char&c:s)c=(char)std::tolower((unsigned char)c); return Value(s); };
    b[BI("push")] = [](std::vector<Value>& a) -> Value { if (a.size()<2||a[0].type()!=Value::ARRAY) throw std::runtime_error("push() requires array and value"); a[0].as_array()->push_back(a[1]); return a[0]; };
    b[BI("pop")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) throw std::runtime_error("pop() requires array"); auto ar=a[0].as_array(); if (ar->empty()) return Value(); Value l=ar->back(); ar->pop_back(); return l; };
    b[BI("join")] = [](std::vector<Value>& a) -> Value { if (a.empty()||a[0].type()!=Value::ARRAY) return Value(a.empty()?std::string():a[0].to_string()); std::string sep=a.size()>=2?a[1].to_string():""; std::string r; auto& ar=*a[0].as_array(); for(size_t i=0;i<ar.size();++i){ if(i)r+=sep; r+=ar[i].to_string(); } return Value(r); };
    b[BI("stop")] = [](std::vector<Value>&) -> Value { return Value(); };
    // exit()/die() — CLI-VM'de bağlı değildi ("Not callable" fırlatıyordu).
    // interpreter ile birebir: ilk argüman INT ise çıkış kodu, yoksa 0.
    {
        auto do_exit = [](std::vector<Value>& a) -> Value {
            int code = 0;
            if (!a.empty() && a[0].type() == Value::INT) code = (int)a[0].as_int();
            throw look::ExitException(code);
        };
        if (look::builtin_index("exit") >= 0) b[BI("exit")] = do_exit;
        if (look::builtin_index("die")  >= 0) b[BI("die")]  = do_exit;
    }
    b[BI("before_route")] = [](std::vector<Value>&) -> Value { return Value(); };
    b[BI("env")] = [](std::vector<Value>& a) -> Value { if (a.empty()) return Value(); const char* e=std::getenv(a[0].to_string().c_str()); if (e) return Value(std::string(e)); return a.size()>=2?Value(a[1].to_string()):Value(std::string()); };
    if (look::builtin_index("args") >= 0)
        b[BI("args")] = [](std::vector<Value>&) -> Value {
            auto arr = std::make_shared<std::vector<Value>>();
            for (const auto& s : look::script_args()) arr->push_back(Value(s));
            return Value(arr);
        };
    // channel() — CLI-VM'de BAĞLI DEĞİLDİ: send/receive'in CHAN_* opcode'ları var ama
    // kanal OLUŞTURMA düz builtin çağrısı → VM tablosunda boş → "kullanilamiyor
    // (baglanmamis)". tree-walk (interpreter.cpp:1631) inline hallettiği için CLI'da
    // parallel+channel tree-walk'ta çalışıp VM'de (DEFAULT) çöküyordu. Semantik birebir:
    // default 128, n<0 hata, n==0 → unbuffered ((size_t)-1).
    if (look::builtin_index("channel") >= 0)
        b[BI("channel")] = [](std::vector<Value>& a) -> Value {
            size_t cap = 128;
            if (!a.empty()) {
                int n = a[0].to_int();
                if (n < 0) throw std::runtime_error("channel: capacity cannot be negative");
                cap = (n == 0) ? (size_t)-1 : (size_t)n;
            }
            return Value(std::make_shared<look::LookChannel>(cap));
        };
    // config() — channel ile AYNI SINIF: web'de (http_main) bağlı, CLI-VM'de eksikti →
    // tree-walk "def" dönerken VM "kullanilamiyor" veriyordu. tree-walk (interpreter.cpp:1802)
    // "section.key" → "SECTION_KEY" env anahtarına çevirip g_env_vars+getenv'e bakar; look_get_env
    // AYNI kaynağı kullanır → birebir parite. Bulunamaz+default-yok → null (sentinel ile ayırt).
    if (look::builtin_index("config") >= 0)
        b[BI("config")] = [](std::vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            std::string dk = a[0].to_string();
            auto dot = dk.find('.');
            std::string ek = (dot != std::string::npos) ? (dk.substr(0, dot) + "_" + dk.substr(dot + 1)) : dk;
            std::transform(ek.begin(), ek.end(), ek.begin(), ::toupper);
            if (a.size() >= 2) return Value(look::look_get_env(ek, a[1].to_string()));
            static const std::string NF = std::string("\x01__look_config_nf__");
            std::string v = look::look_get_env(ek, NF);
            return v == NF ? Value() : Value(v);
        };
    // Modül fn'leri: builtin_names'deki her "mod::fn" → interpreter'ın YÜKLÜ modülü.
    // YALNIZCA `use` edilmiş modüller auto-wire olur — interpreter ile parity: interpreter
    // de string::/math::/array:: için `use` gerektirir (json:: gibi her-zaman-açık olanlar
    // ayrı, index-tabanlı wire edilir). Preload YOK — aksi halde VM `use`'suz çalışıp
    // interpreter hata verirdi (divergence).
    const auto& names = look::builtin_names();
    for (size_t i = 0; i < names.size(); ++i) {
        auto pos = names[i].find("::");
        if (pos == std::string::npos) continue;
        auto f = interp.get_module_fn(names[i].substr(0, pos), names[i].substr(pos + 2));
        if (f) b[i] = [f](std::vector<Value>& args) -> Value { std::vector<Value> aa = args; return f(aa); };
    }
    return b;
}

// ── LOOK 2: komut satırında geri çağırmalar da VM'de koşar ────────────────────────────────────
// timer::after/every ve jobs::worker işleyicisi eskiden betiği bütünüyle tree-walk'a düşürüyordu.
// Şimdi her çağrı kendi VM'inde koşar (web'deki gibi: istek başına yeni VM), aynı yerleşik tabloyla.
//   * iş işleyicisi jobs::run içinde, AYNI iş parçacığında koşar → ana VM'in GÜNCEL global'lerini görür
//   * zamanlayıcı başka iş parçacığında koşar → kurulduğu andaki global'lerin kopyasını görür
// Çıktı stdout'a gider. ESKİ HATA: yorumlayıcı işleyicinin/zamanlayıcının çıktısını ayrı bir tampona
// yazıp atıyordu — komut satırındaki bir iş betiğinde `print` sessizce kayboluyordu.
look::Value run_cli_closure(const CliCallbacks& cc, const look::Value& fnv, std::vector<look::Value> args,
                                   const std::unordered_map<std::string, look::Value>* globals,
                                   const char* method, const char* what, bool& ok) {
    ok = false;
    look::WebContext ctx;
    ctx.method = method;
    try {
        look::VM sub(*cc.shared, std::cout);
        if (globals) sub.set_globals(*globals);
        else if (cc.main_vm) sub.set_globals(cc.main_vm->get_globals());
        sub.set_web_context(&ctx);
        look::Value r = sub.call_closure(*fnv.as_bytecode_fn(), std::move(args));
        std::cout.flush();
        ok = true;
        return r;
    } catch (const look::ExitException&) {
        throw;
    } catch (const std::exception& ex) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "VM", std::string(what) + " error: " + ex.what());
    } catch (...) {
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "VM", std::string(what) + " unknown error");
    }
    return look::Value();
}
void wire_cli_callbacks(std::vector<look::BuiltinFn>& b, look::Interpreter& interp,
                               std::shared_ptr<CliCallbacks> cc) {
    using look::Value;
    auto BI = [](const char* n) { return (size_t)look::builtin_index(n); };
    // jobs::run işleyicileri vm_closure_runner üzerinden çağırır (BYTECODE_FN).
    look::vm_closure_runner() = [cc](const Value& fn, std::vector<Value> args, const char* what, bool& ok) {
        return run_cli_closure(*cc, fn, std::move(args), nullptr, "__JOB__", what, ok);
    };
    b[BI("jobs::run")] = [&interp](std::vector<Value>& a) -> Value {
        return interp.jobs_run(a.empty() ? 5000 : (int)a[0].to_float());
    };
    if (auto f = interp.get_module_fn("jobs", "worker"))
        b[BI("jobs::worker")] = [f](std::vector<Value>& a) -> Value {
            std::vector<Value> aa = a;
            if (aa.size() >= 2 && aa[1].type() == Value::BYTECODE_FN) aa[1] = aa[1].clone_for_thread();
            return f(aa);
        };
    for (const char* name : {"timer::after", "timer::every"}) {
        const bool repeat = std::string(name) == "timer::every";
        const std::string nm = name;
        b[BI(name)] = [cc, repeat, nm](std::vector<Value>& a) -> Value {
            if (a.size() < 2) throw std::runtime_error(nm + "() takes 2 arguments");
            if (a[1].type() != Value::BYTECODE_FN) throw std::runtime_error(nm + "() second argument must be a function");
            // Zamanlayıcı başka iş parçacığında koşar: global'lerin o anki kopyası (paylaşım yok).
            auto snap = std::make_shared<std::unordered_map<std::string, Value>>();
            if (cc->main_vm)
                for (const auto& [k, v] : cc->main_vm->get_globals()) snap->emplace(k, v.clone_for_thread());
            Value fnv = a[1].clone_for_thread();
            auto cb = [cc, fnv, snap]() {
                bool ok = false;
                run_cli_closure(*cc, fnv, {}, snap.get(), "__TIMER__", "timer callback", ok);
            };
            const int ms = (int)a[0].to_int();
            return Value(repeat ? look::TimerManager::instance().every(ms, std::move(cb))
                                : look::TimerManager::instance().after(ms, std::move(cb)));
        };
    }
    b[BI("timer::cancel")] = [](std::vector<Value>& a) -> Value {
        if (a.empty()) throw std::runtime_error("timer::cancel() takes 1 argument");
        look::TimerManager::instance().cancel((int)a[0].to_int());
        return Value();
    };
    // timer::wait([ms=5000]) — parallel::wait ile aynı biçim: tek seferlik zamanlayıcılar tetiklenip
    // geri çağırmaları bitene kadar bekler; true = hepsi bitti, false = süre doldu.
    b[BI("timer::wait")] = [](std::vector<Value>& a) -> Value {
        int ms = 5000;
        if (!a.empty() && a[0].type() == Value::INT) ms = (int)a[0].as_int();
        return Value(look::TimerManager::instance().wait_one_shots(ms));
    };
    // ws::/sse:: yalnız bir web uygulamasının bağlantı rotasında anlamlı — açık hata.
    for (const auto& n : look::builtin_names()) {
        if (n.rfind("ws::", 0) != 0 && n.rfind("sse::", 0) != 0) continue;
        b[BI(n.c_str())] = [n](std::vector<Value>&) -> Value {
            throw std::runtime_error(n + "() can be used only in a web application (lk-fcgi), not on the command line");
        };
    }
}

} // namespace look

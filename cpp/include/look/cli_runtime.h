#pragma once
// Komut satırı VM ortamı — `lk` ve `lk test` AYNI yerleşik tabloyu ve geri çağırma bağlamalarını
// kullanır (src/cli_runtime.cpp). Testler programın gerçekte koştuğu ortamı sınasın diye tek kaynak.
#include "look/interpreter.h"
#include "look/vm.h"
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace look {

// Yerleşik tablo: `use` edilmiş modüllerin fonksiyonları (interp'ten) + çıplak yerleşikler; print/write → out.
std::vector<BuiltinFn> build_cli_builtins(Interpreter& interp, std::ostream& out);

// İş işleyicisi / zamanlayıcı / parallel geri çağırmaları için paylaşılan durum.
struct CliCallbacks {
    VM::SharedState* shared  = nullptr;   // yerleşik tablo (süreç ömrü)
    VM*              main_vm = nullptr;   // jobs::run anında güncel global'ler
};

// Bir VM closure'ını kendi VM'inde çalıştırır (hata loglanır, ok=false). globals boşsa ana VM'inkiler.
Value run_cli_closure(const CliCallbacks& cc, const Value& fnv, std::vector<Value> args,
                      const std::unordered_map<std::string, Value>* globals,
                      const char* method, const char* what, bool& ok);

// timer::/jobs::worker/jobs::run komut satırında VM'e; ws::/sse:: açık hata.
void wire_cli_callbacks(std::vector<BuiltinFn>& b, Interpreter& interp, std::shared_ptr<CliCallbacks> cc);

} // namespace look

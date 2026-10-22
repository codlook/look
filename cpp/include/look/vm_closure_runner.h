#pragma once
// Bir VM closure'ını YENİ BİR İSTEK gibi çalıştırır (kendi VM'i, global'lerin bildirilen
// değerleri): zamanlayıcı geri çağırmaları ve jobs::run işleyicileri bunu kullanır. Web sunucusu
// ve komut satırı (cli_runtime) kurar. ok=false ise hata loglanmıştır. Kurulmamışsa boştur.
#include "look/value.h"
#include <functional>
#include <vector>

namespace look {

using VmClosureRunner = std::function<Value(const Value& fn, std::vector<Value> args, const char* what, bool& ok)>;
inline VmClosureRunner& vm_closure_runner() { static VmClosureRunner r; return r; }

} // namespace look

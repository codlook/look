#pragma once
#include "look/interpreter.h"   // Value

namespace look {

// count() / len() semantiği — TEK TANIM (interpreter + CLI VM + web VM aynı sonucu versin).
//
// ESKİ HATA (VM↔interpreter ayrışması, differential-görünmez çünkü testler DÜZ dizi kullanıyordu):
// interpreter.cpp assoc sentinel'ini sayıyordu ama üç VM builtin tablosu (main.cpp b[2],
// http_main setup_builtins[2] + req_builtins[2]) ATLIYORDU → count(["x"=>1]) = 3 (sentinel+2)
// döndürüyordu, 1 yerine. Sonuç: `if (count($assoc)==0)` hiç true olmuyordu, count(request::json())
// sessizce yanlıştı. Assoc = ["__assoc__", k0,v0,...] → çift sayısı = (size-1)/2.
//
// Bu yardımcı dört çağrı yerini tek tanıma indirir (path_guard.h / format.h / html_escape.h deseni).
// KÖK: sentinel tasarımının kendisi ([[look]] Value::Type::ASSOC ayrı tip bunu tümden kapatır).
inline int look_count(const Value& v) {
    if (v.type() == Value::ARRAY) {
        auto& arr = *v.as_array();
        if (!arr.empty() && arr[0].type() == Value::STRING && arr[0].as_string() == "__assoc__")
            return (int)((arr.size() - 1) / 2);
        return (int)arr.size();
    }
    if (v.type() == Value::STRING) return (int)v.as_string().size();
    return 0;
}

} // namespace look

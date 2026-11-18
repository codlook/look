#pragma once
#include "look/runtime_host.h"   // Value

namespace look {

// count() / len() semantiği — TEK TANIM (CLI VM + web VM aynı sonucu versin).
//
// ESKİ HATA (VM↔interpreter ayrışması, differential-görünmez çünkü testler DÜZ dizi kullanıyordu):
// eski yorumlayıcı assoc sentinel'ini sayıyordu ama üç VM builtin tablosu (main.cpp b[2],
// http_main setup_builtins[2] + req_builtins[2]) ATLIYORDU → count(["x"=>1]) = 3 (sentinel+2)
// döndürüyordu, 1 yerine. Sonuç: `if (count($assoc)==0)` hiç true olmuyordu, count(request::json())
// sessizce yanlıştı. Assoc = ["__assoc__", k0,v0,...] → çift sayısı = (size-1)/2.
//
// Bu yardımcı dört çağrı yerini tek tanıma indirir (path_guard.h / format.h / html_escape.h deseni).
// KÖK: sentinel tasarımının kendisi ([[look]] Value::Type::ASSOC ayrı tip bunu tümden kapatır).
inline int look_count(const Value& v) {
    if (v.type() == Value::ARRAY) {
        auto& arr = *v.as_array();
        if (!arr.empty() && arr[0].is_assoc_marker())
            return (int)((arr.size() - 1) / 2);
        return (int)arr.size();
    }
    // Metin: KARAKTER (kod noktası) sayısı — string::len ile aynı. `len` derleyicide count'a eşlenir
    // ve belgede string::len'in takma adıdır; eskiden bayt sayıyordu: len("éé") = 4 (rastgele
    // karşılaştırmalı test buldu). Devam baytı (10xxxxxx) sayılmaz.
    if (v.type() == Value::STRING) {
        int n = 0;
        for (unsigned char c : v.as_string()) if ((c & 0xC0) != 0x80) ++n;
        return n;
    }
    // LOOK 2: struct bir dizi değildir — eskiden sessizce 0 dönüyordu.
    if (v.type() == Value::STRUCT)
        throw std::runtime_error("count() does not take a struct ('" + (*v.as_struct())[0].str_ref()
                                 + "'): a struct is not an array, use its fields");
    return 0;
}

} // namespace look

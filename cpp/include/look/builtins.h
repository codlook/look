#pragma once

// builtins.h — VM builtin tablosunun TEK kaynağı
//
// Bu tablo iki tüketiciyi senkron tutar:
//   1. compiler.cpp  — isim → builtin index (CALL_BUILTIN opcode'u üretir)
//   2. http_main.cpp — index → çalışma zamanı fonksiyonu (req_builtins wiring)
//
// Kural: "mod::fn" biçimindeki isimler http_main tarafında OTOMATİK wire edilir
// (interpreter'ın modül fonksiyonu üzerinden). "::" içermeyen isimler (print,
// count, strlen, route...) özel semantik taşır ve http_main'de elle bağlanır.
//
// YENİ BUILTIN EKLEME: listenin SONUNA ekle (mevcut indeksler .lkc bytecode
// cache'lerinde gömülü — araya ekleme tüm cache'leri bozar). "mod::fn" ise
// başka hiçbir şey gerekmez; özel isim ise http_main'de lambda ekle.

#include <string>
#include <vector>

namespace look {

// Kanonik builtin isim listesi — index == vector pozisyonu
const std::vector<std::string>& builtin_names();

// İsim → index; bulunamazsa -1. O(1) (ilk çağrıda hash map kurulur).
int builtin_index(const std::string& name);

} // namespace look

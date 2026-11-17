#pragma once
// max/min'in TEK tanımı (çekirdek max()/min() iki motorda ve math::max/min). Tek liste → en büyük/küçük
// eleman, boş liste → null; birden çok argüman → aralarından seçer. ESKİ: çekirdek max([3,9,2]) listenin
// KENDİSİNİ döndürüyordu ve üç argümanda yalnız ilk ikisini karşılaştırıyordu (max(1,2,8) = 2) — sessiz
// yanlış sonuç; belge max([5,3,8]) → 8 diyordu. Eşitlikte SON geçen döner (math::max'in eski davranışı).
#include "look/interpreter.h"
#include <vector>

namespace look {

inline Value num_extreme(const std::vector<Value>& args, bool want_max) {
    auto better = [want_max](const Value& a, const Value& best) { return want_max ? a >= best : a <= best; };
    if (args.size() == 1 && args[0].type() == Value::ARRAY) {
        const auto arr = args[0].as_array();
        if (!arr || arr->empty()) return Value();
        Value best = (*arr)[0];
        for (size_t i = 1; i < arr->size(); ++i) if (better((*arr)[i], best)) best = (*arr)[i];
        return best;
    }
    if (args.empty()) return Value();
    Value best = args[0];
    for (size_t i = 1; i < args.size(); ++i) if (better(args[i], best)) best = args[i];
    return best;
}

} // namespace look

#pragma once
// İkili operatör zincirinin sol omurgası. `a + b + c + …` ((a+b)+c)+… diye sola doğru iç içedir;
// ayrıştırıcı 1.000 operatöre kadar izin verir. Ağacı gezen kod sol dala özyinelemeyle inerse zincir
// uzunluğu kadar C++ çerçevesi harcar (ölçüldü: 512 KB yığında 1.000 terim ayrıştırma sonrası
// denetimde durdu, derleyicide çöktü). Gezen kod omurgayı bununla DÖNGÜYLE dolaşır; sıra özyinelemeli
// gezintiyle aynıdır: en soldaki terim, sonra içten dışa her düğümün sağı.
#include "look/ast.h"
#include <vector>

namespace look {

// spine: dıştan içe ikili düğümler (spine[0] == b). Dönüş: en soldaki (ikili olmayan) terim.
inline const Expression* binary_left_spine(const BinaryExpression* b, std::vector<const BinaryExpression*>& spine) {
    const Expression* cur = b;
    while (auto* x = node_as<BinaryExpression>(cur)) { spine.push_back(x); cur = x->left.get(); }
    return cur;
}

} // namespace look

#pragma once
// Left spine of a binary operator chain. `a + b + c + …` nests to the left as ((a+b)+c)+…;
// the parser allows up to 1,000 operators. If code that walks the tree descends the left branch recursively it uses
// as many C++ frames as the chain length (measured: on a 512 KB stack a 1,000-term chain passed parsing, stopped
// in the check and crashed in the compiler). Walking code goes around the spine with a LOOP; the order is the same as a recursive
// walk: the leftmost term, then from inside out the right side of each node.
#include "look/ast.h"
#include <vector>

namespace look {

// spine: binary nodes from outside in (spine[0] == b). Returns: the leftmost (non-binary) term.
inline const Expression* binary_left_spine(const BinaryExpression* b, std::vector<const BinaryExpression*>& spine) {
    const Expression* cur = b;
    while (auto* x = node_as<BinaryExpression>(cur)) { spine.push_back(x); cur = x->left.get(); }
    return cur;
}

} // namespace look

// tests/builtin_effect_table_test.sh compiles this: does every built-in (builtin_names) have exactly one effect class,
// and is there a name in the classification that is not a built-in (a typo)? A new built-in without a class → red.
#include "look/builtins.h"
#include <iostream>
#include <map>
int main() {
    int bad = 0;
    std::map<std::string, int> seen;
    for (const auto& n : look::builtin_names()) {
        if (++seen[n] > 1) continue;   // a duplicate name in the table (join) is counted once
        if (look::builtin_effect(n) == look::BuiltinEffect::Unknown) {
            std::cout << "unclassified built-in: " << n << "\n"; ++bad;
        }
    }
    for (const auto& s : look::builtin_effect_strays()) { std::cout << "classified but not a built-in: " << s << "\n"; ++bad; }
    std::cout << seen.size() << " built-ins, " << bad << " problem(s)\n";
    return bad ? 1 : 0;
}

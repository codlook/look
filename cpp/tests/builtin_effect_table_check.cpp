// tests/builtin_effect_table_test.sh derler: her yerleşik (builtin_names) tam bir etki sınıfına sahip mi,
// sınıflandırmada yerleşik olmayan (yazım hatası) ad var mı? Sınıfsız yeni yerleşik → kırmızı.
#include "look/builtins.h"
#include <iostream>
#include <map>
int main() {
    int bad = 0;
    std::map<std::string, int> seen;
    for (const auto& n : look::builtin_names()) {
        if (++seen[n] > 1) continue;   // tabloda yinelenen ad (join) bir kez sayılır
        if (look::builtin_effect(n) == look::BuiltinEffect::Unknown) {
            std::cout << "unclassified built-in: " << n << "\n"; ++bad;
        }
    }
    for (const auto& s : look::builtin_effect_strays()) { std::cout << "classified but not a built-in: " << s << "\n"; ++bad; }
    std::cout << seen.size() << " built-ins, " << bad << " problem(s)\n";
    return bad ? 1 : 0;
}

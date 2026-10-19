#pragma once
// Programın `use <modül>` satırlarını toplar — `use "dosya.lk"` ile dahil edilen dosyalarınkiler de
// (özyinelemeli). CLI ve web aynı listeyi kullanır: yalnız `use` edilen stdlib modülleri açılır,
// `use` edilmeyen modülün fonksiyonu "Module 'x' not loaded." verir (iki yolda aynı kural).
// Açılamayan/ayrıştırılamayan dahil dosya burada sorun sayılmaz — derleyici aynı dosyada nedenini
// söyleyerek durur.
#include "look/ast.h"
#include "look/lexer.h"
#include "look/parser.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace look {

inline void collect_use_modules(const Program& prog, const std::filesystem::path& dir,
                                std::set<std::string>& seen_files, std::vector<std::string>& out) {
    for (auto& s : prog.statements) {
        if (auto* u = dynamic_cast<const UseStatement*>(s.get())) {
            out.push_back(u->module_name);
        } else if (auto* uf = dynamic_cast<const UseFileStatement*>(s.get())) {
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::weakly_canonical(dir / uf->path, ec);
            if (ec || !seen_files.insert(abs.string()).second) continue;
            std::ifstream f(abs);
            if (!f) continue;
            std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            try {
                Lexer lx(src);
                Parser p(lx.scan_tokens());
                auto sub = p.parse();
                collect_use_modules(*sub, abs.parent_path(), seen_files, out);
            } catch (...) { continue; }
        }
    }
}

} // namespace look

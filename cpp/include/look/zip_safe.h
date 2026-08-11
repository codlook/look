#pragma once
// Zip-Slip containment kararı — saf predicate (installer.cpp'den "dikişi aç"). Gerçek extract/
// miniz OLMADAN tablo-test edilir; güvenlik-kararının regresyonunu ağsız kilitler.
//
// Bir zip entry (filename) dest_dir İÇİNE mi açılacak? Normalize edilmiş yol dest_dir'de
// kalmalı. DÜZ string-prefix YETMEZ: "/pkg/user/repo" öneki "/pkg/user/repo-evil" ile de
// eşleşir (sibling-prefix escape → dest DIŞINA yazma). Önek + AYIRICI-SINIRI zorunlu.
#include <string>
#include <filesystem>

namespace look {

inline bool zip_entry_inside_dest(const std::string& filename,
                                  const std::filesystem::path& dest_dir) {
    namespace fs = std::filesystem;
    fs::path safe_dest = fs::weakly_canonical(dest_dir);
    fs::path out_path  = fs::weakly_canonical(dest_dir / filename);
    const std::string dest_str = safe_dest.string();
    const std::string out_str  = out_path.string();
    return out_str.size() >= dest_str.size() &&
           out_str.compare(0, dest_str.size(), dest_str) == 0 &&
           (out_str.size() == dest_str.size() ||
            out_str[dest_str.size()] == '/' ||
            out_str[dest_str.size()] == '\\');
}

} // namespace look

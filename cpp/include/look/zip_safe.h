#pragma once
// Zip-Slip containment kararı — saf predicate (installer.cpp'den "dikişi aç"). Gerçek extract/
// miniz OLMADAN tablo-test edilir; güvenlik-kararının regresyonunu ağsız kilitler.
//
// Bir zip entry (filename) dest_dir İÇİNE mi açılacak? Normalize edilmiş yol dest_dir'de
// kalmalı. DÜZ string-prefix YETMEZ: "/pkg/user/repo" öneki "/pkg/user/repo-evil" ile de
// eşleşir (sibling-prefix escape → dest DIŞINA yazma). Önek + AYIRICI-SINIRI zorunlu.
#include <string>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace look {

// ZIP bombası kararı: arşivin merkez dizinindeki girdi sayısı ve bildirilen açılmış boyutları.
// miniz açarken gerçek boyutu bildirilene karşı doğrular (fazlası hatadır), yani bildirilen toplam
// gerçek üst sınırdır. Boş dönüş = kabul; dolu = kullanıcıya gösterilecek İngilizce neden.
inline constexpr uint64_t ZIP_MAX_TOTAL   = 256ull * 1024 * 1024;   // paketler kaynak koddur
inline constexpr size_t   ZIP_MAX_ENTRIES = 20000;
inline std::string zip_size_problem(const std::vector<uint64_t>& uncomp_sizes) {
    if (uncomp_sizes.size() > ZIP_MAX_ENTRIES)
        return "the archive has more than " + std::to_string(ZIP_MAX_ENTRIES) + " entries";
    uint64_t total = 0;
    for (uint64_t s : uncomp_sizes) {
        if (s > ZIP_MAX_TOTAL - total)   // taşma-güvenli toplam
            return "the archive unpacks to more than " + std::to_string(ZIP_MAX_TOTAL >> 20) + " MB";
        total += s;
    }
    return "";
}

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

// Kanonik path-altında-kök kontrolü — string tabanlı traversal kontrolünün göremediği
// SYMLINK'i ve upstream-decode varsayımını ortadan kaldırır. Tek tanım; cgi_main.cpp,
// fcgi_main.cpp (ve konsept olarak file_stdlib.cpp assert_in_file_root) aynı deseni paylaşır.
//
// String kontrolu ("../" ara) iki sebeple yetersiz:
//  1. symlink: doc/rapor.lk -> /baska-kiraci/config.lk yolunda ".." YOK ama doc disina cikar.
//  2. upstream-decode: guard'a %2e mi ".." mi ulastigi Apache/nginx/IIS'e gore degisir.
// weakly_canonical hem symlink'i cozer hem yolu normalize eder -> kodlama/symlink ne olursa
// olsun karar KANONIK yola gore verilir. Bilesen-bazli karsilastirma "/srv/app" vs
// "/srv/app-evil" string-prefix atlatmasini da kapatir.
#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace look {

inline bool path_under_root(const std::string& root, const std::string& cand) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path r = fs::weakly_canonical(fs::absolute(fs::path(root), ec), ec);
    if (ec) return false;
    fs::path c = fs::weakly_canonical(fs::absolute(fs::path(cand), ec), ec);
    if (ec) return false;
    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci)
        if (ci == c.end() || *ci != *ri) return false;   // bilesen bazli prefix
    return true;
}

} // namespace look

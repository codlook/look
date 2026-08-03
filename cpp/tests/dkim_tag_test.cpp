// dkim_tag ① KAT — RFC 6376 tag ayristirma dogru mu? Saf parser (look/dkim_tag.h).
// Derle:  g++ -std=c++17 -I../include dkim_tag_test.cpp -o dkim_tag_test && ./dkim_tag_test
// exit 0 = gecti, exit 1 = ① geri geldi.
#include "look/dkim_tag.h"
#include <string>
#include <iostream>

// ESKI hatali parser (find-bazli) — POZITIF KONTROL icin: ①'de yanlis SONUC vermeli.
static std::string old_tag(const std::string& hdr, const std::string& name) {
    std::string pat = name + "=";
    size_t p = hdr.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = hdr.find_first_of(";", p);
    return (e != std::string::npos) ? hdr.substr(p, e - p) : hdr.substr(p);
}

int main() {
    int fail = 0, run = 0;
    auto check = [&](const std::string& label, bool ok) {
        run++;
        std::cout << (ok ? "PASS  " : "FAIL  ") << label << "\n";
        if (!ok) fail++;
    };

    // DKIM-Signature: bh= h='den ONCE (RFC tag sirasini zorunlu KILMAZ; saldirgan sirar).
    std::string hdr =
        "DKIM-Signature: v=1; a=rsa-sha256; d=example.com; s=sel; "
        "bh=BODYHASHvalue123; h=from:to:subject; b=SIGNATUREvalue456";

    // ① CEKIRDEK: tag("h") imzalanan-baslik-listesini dondurmeli, bh= degerini DEGIL.
    check("dkim_tag(h) = from:to:subject (bh degil)",
          look::dkim_tag(hdr, "h") == "from:to:subject");
    check("dkim_tag(bh) = body hash",
          look::dkim_tag(hdr, "bh") == "BODYHASHvalue123");
    check("dkim_tag(h) != dkim_tag(bh) (ayri)",
          look::dkim_tag(hdr, "h") != look::dkim_tag(hdr, "bh"));
    check("dkim_tag(d) = example.com",  look::dkim_tag(hdr, "d") == "example.com");
    check("dkim_tag(s) = sel",          look::dkim_tag(hdr, "s") == "sel");
    check("dkim_tag(b) = SIGNATUREvalue456", look::dkim_tag(hdr, "b") == "SIGNATUREvalue456");

    // POZITIF KONTROL: eski find() parser'i ①'i YANLIS yapmali (tag(h), "bh=" icindeki
    // "h="e carpip bh degerini dondurur) → old_tag(h)==old_tag(bh). Bu PASS ise, test
    // ①'i gercekten ayirt ediyor demektir (yeni parser'da ise != olur, yukarida gecti).
    check("pozitif kontrol: ESKI parser ①'de bozuk (old_tag(h)==old_tag(bh))",
          old_tag(hdr, "h") == old_tag(hdr, "bh"));

    std::cout << "\n";
    if (fail == 0) { std::cout << "PASS: " << run << "/" << run << " dkim_tag KAT (①)\n"; return 0; }
    std::cout << "FAIL: " << fail << "/" << run << " dkim_tag\n"; return 1;
}

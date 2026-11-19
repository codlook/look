// dkim_tag ① KAT — is RFC 6376 tag parsing correct? Pure parser (look/dkim_tag.h).
// Build:  g++ -std=c++17 -I../include dkim_tag_test.cpp -o dkim_tag_test && ./dkim_tag_test
// exit 0 = passed, exit 1 = ① is back.
#include "look/dkim_tag.h"
#include <string>
#include <iostream>

// OLD buggy parser (find-based) — for the POSITIVE CONTROL: must give a WRONG RESULT on ①.
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

    // DKIM-Signature: bh= BEFORE h= (RFC does NOT REQUIRE a tag order; the attacker orders it).
    std::string hdr =
        "DKIM-Signature: v=1; a=rsa-sha256; d=example.com; s=sel; "
        "bh=BODYHASHvalue123; h=from:to:subject; b=SIGNATUREvalue456";

    // ① CORE: tag("h") must return the signed-header list, NOT the bh= value.
    check("dkim_tag(h) = from:to:subject (bh degil)",
          look::dkim_tag(hdr, "h") == "from:to:subject");
    check("dkim_tag(bh) = body hash",
          look::dkim_tag(hdr, "bh") == "BODYHASHvalue123");
    check("dkim_tag(h) != dkim_tag(bh) (ayri)",
          look::dkim_tag(hdr, "h") != look::dkim_tag(hdr, "bh"));
    check("dkim_tag(d) = example.com",  look::dkim_tag(hdr, "d") == "example.com");
    check("dkim_tag(s) = sel",          look::dkim_tag(hdr, "s") == "sel");
    check("dkim_tag(b) = SIGNATUREvalue456", look::dkim_tag(hdr, "b") == "SIGNATUREvalue456");

    // POSITIVE CONTROL: the old find() parser must get ① WRONG (tag(h) hits the "h=" inside "bh="
    // and returns the bh value) → old_tag(h)==old_tag(bh). If this PASSES, the test really
    // tells ① apart (with the new parser it is != , passed above).
    check("pozitif kontrol: ESKI parser ①'de bozuk (old_tag(h)==old_tag(bh))",
          old_tag(hdr, "h") == old_tag(hdr, "bh"));

    std::cout << "\n";
    if (fail == 0) { std::cout << "PASS: " << run << "/" << run << " dkim_tag KAT (①)\n"; return 0; }
    std::cout << "FAIL: " << fail << "/" << run << " dkim_tag\n"; return 1;
}

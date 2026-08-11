// SHA-256 / HMAC / PBKDF2 known-answer test — look/crypto_sha256.h (dikişi aç).
// Bu üçlü JWT(HS256)/DKIM/session/auth'u besler. RFC vektörleriyle "kendi çıktısına değil,
// SPEC'e pinle": NIST SHA-256 · RFC 4231 HMAC-SHA256 · RFC 7914 §11 PBKDF2-HMAC-SHA256.
// POZİTİF-KONTROLLÜ (impl bir bit kayarsa vektör tutmaz → RED). Yeni builtin YOK.
//   Build: g++ -std=c++17 -Iinclude tests/crypto_sha256_test.cpp -o /tmp/csha && /tmp/csha
#include "look/crypto_sha256.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace look;

static int fails = 0, ran = 0;
static std::vector<uint8_t> B(const std::string& s){ return std::vector<uint8_t>(s.begin(), s.end()); }
static std::string hex(const std::vector<uint8_t>& v){
    static const char* H="0123456789abcdef"; std::string o;
    for(uint8_t b: v){ o+=H[b>>4]; o+=H[b&0xF]; } return o;
}
static void chk(const char* label, const std::string& got, const char* want){
    ran++;
    bool ok = (got == want);
    printf("  %-32s %s\n", label, ok ? "OK" : "FAIL");
    if(!ok){ printf("      got  %s\n      want %s\n", got.c_str(), want); fails++; }
}
static std::vector<uint8_t> rep(uint8_t b, size_t n){ return std::vector<uint8_t>(n, b); }

int main(){
    printf("SHA-256 / HMAC / PBKDF2 RFC known-answer:\n");

    // ── SHA-256 (NIST FIPS 180-4 örnekleri) ──
    chk("sha256(\"abc\")", hex(sha256(B("abc"))),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    chk("sha256(\"\")", hex(sha256(B(""))),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    chk("sha256(56-char)", hex(sha256(B("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // ── HMAC-SHA256 (RFC 4231) ──
    // Test Case 1: key=0x0b×20, data="Hi There"
    chk("hmac RFC4231 #1", hex(hmac_sha256(rep(0x0b,20), B("Hi There"))),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // Test Case 2: key="Jefe", data="what do ya want for nothing?"
    chk("hmac RFC4231 #2", hex(hmac_sha256(B("Jefe"), B("what do ya want for nothing?"))),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // Test Case 3: key=0xaa×20, data=0xdd×50
    chk("hmac RFC4231 #3", hex(hmac_sha256(rep(0xaa,20), rep(0xdd,50))),
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // ── PBKDF2-HMAC-SHA256 (RFC 7914 §11) ──
    // P="passwd" S="salt" c=1 dkLen=64
    chk("pbkdf2 passwd/salt/1/64", hex(pbkdf2_sha256("passwd", B("salt"), 1, 64)),
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
        "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    // P="Password" S="NaCl" c=80000 dkLen=64
    chk("pbkdf2 Password/NaCl/80000/64", hex(pbkdf2_sha256("Password", B("NaCl"), 80000, 64)),
        "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
        "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
    // RFC 6070-türevi kısa: P="password" S="salt" c=1 dkLen=32 (tek-block sınır)
    chk("pbkdf2 password/salt/1/32", hex(pbkdf2_sha256("password", B("salt"), 1, 32)),
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    // Çok-iterasyon tek-block: c=4096 dkLen=32
    chk("pbkdf2 password/salt/4096/32", hex(pbkdf2_sha256("password", B("salt"), 4096, 32)),
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    const int EXPECTED = 10;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (SHA-256/HMAC/PBKDF2 RFC-vektörlerine pinli)\n", fails);
    return fails ? 1 : 0;
}

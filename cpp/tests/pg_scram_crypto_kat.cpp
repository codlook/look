// PG SCRAM crypto oracle — GERÇEK PostgresClient::sha256/hmac_sha256/pbkdf2_sha256'ı aynı RFC
// vektörlerine pinler. Birleştirme SONRASI bu üçlü look::crypto_sha256.h'ye array<32>↔vector
// adaptörüyle delege eder; test artık REGRESYON GUARD'ı: adaptörler doğru bağlı + PG'nin tek-block
// (dkLen=32) yolu RFC'ye pinli kalmalı. Bir bayt kayarsa vektör tutmaz → RED (pozitif-kontrol).
// PG yalnız TEK-BLOCK yol taşır (array<32>): dkLen=32 vakaları PG'nin canlı auth yolunu birebir
// egzersiz eder. Private static'lere `#define private public` ile eriş (test-only trick).
//   Build (VPS): g++ -std=c++17 -Iinclude tests/pg_scram_crypto_kat.cpp src/postgres_client.cpp \
//                -lssl -lcrypto -o /tmp/pgkat && /tmp/pgkat
#define private public
#include "look/postgres_client.h"
#undef private
#include <cstdio>
#include <string>
#include <vector>
#include <array>
using look::PostgresClient;

static int fails = 0, ran = 0;
static std::string hex(const uint8_t* v, size_t n){
    static const char* H="0123456789abcdef"; std::string o;
    for(size_t i=0;i<n;i++){ o+=H[v[i]>>4]; o+=H[v[i]&0xF]; } return o;
}
static std::string hex32(const std::array<uint8_t,32>& a){ return hex(a.data(), 32); }
static void chk(const char* label, const std::string& got, const char* want){
    ran++;
    bool ok = (got == want);
    printf("  %-34s %s\n", label, ok ? "OK" : "FAIL");
    if(!ok){ printf("      got  %s\n      want %s\n", got.c_str(), want); fails++; }
}

int main(){
    printf("PG SCRAM crypto (PostgresClient::) RFC known-answer:\n");

    // ── SHA-256 (NIST FIPS 180-4) ──
    { std::string m="abc";
      chk("pg sha256(\"abc\")", hex32(PostgresClient::sha256((const uint8_t*)m.data(), m.size())),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
    { std::string m="";
      chk("pg sha256(\"\")", hex32(PostgresClient::sha256((const uint8_t*)m.data(), m.size())),
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"); }
    { std::string m="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
      chk("pg sha256(56-char)", hex32(PostgresClient::sha256((const uint8_t*)m.data(), m.size())),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"); }

    // ── HMAC-SHA256 (RFC 4231) ──
    { std::vector<uint8_t> k(20,0x0b); std::string d="Hi There";
      chk("pg hmac RFC4231 #1", hex32(PostgresClient::hmac_sha256(k.data(),k.size(),(const uint8_t*)d.data(),d.size())),
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"); }
    { std::string k="Jefe"; std::string d="what do ya want for nothing?";
      chk("pg hmac RFC4231 #2", hex32(PostgresClient::hmac_sha256((const uint8_t*)k.data(),k.size(),(const uint8_t*)d.data(),d.size())),
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"); }
    { std::vector<uint8_t> k(20,0xaa); std::vector<uint8_t> d(50,0xdd);
      chk("pg hmac RFC4231 #3", hex32(PostgresClient::hmac_sha256(k.data(),k.size(),d.data(),d.size())),
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"); }

    // ── PBKDF2-HMAC-SHA256 tek-block (dkLen=32, PG'nin CANLI auth yolu) ──
    // RFC 6070-türevi: P="password" S="salt" c=1  → tek-block, INT(1) big-endian
    { std::string s="salt";
      chk("pg pbkdf2 password/salt/1/32", hex32(PostgresClient::pbkdf2_sha256("password",(const uint8_t*)s.data(),s.size(),1)),
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"); }
    // Çok-iterasyon tek-block: c=4096 (SCRAM iter sayısına yakın gerçekçi yük)
    { std::string s="salt";
      chk("pg pbkdf2 password/salt/4096/32", hex32(PostgresClient::pbkdf2_sha256("password",(const uint8_t*)s.data(),s.size(),4096)),
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"); }

    const int EXPECTED = 8;
    if (ran != EXPECTED) { printf("\nFAIL: %d vaka beklendi, %d koştu\n", EXPECTED, ran); return 1; }
    printf(fails ? "\n%d FAIL — PG kopyası RFC'den AYRIŞIYOR (BULGU)\n"
                 : "\nPG kopyası RFC'ye pinli — look::crypto_sha256.h ile AYRIŞMIYOR (birleştirme güvenli)\n", fails);
    return fails ? 1 : 0;
}

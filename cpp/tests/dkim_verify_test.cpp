// DKIM verify — pozitif kontrol + ② çoklu-From tablosu (seam: dkim_verify_with_key).
// Hipotez (ölçülecek, VARSAYILMAYACAK): alignment find(ilk From) vs crypto rfind(son From)
// tutarsızlığı sömürülebilir mi, yoksa fail-safe mi (crypto imzalanana bağlı → mismatch → red)?
//   Build (VPS): g++ -std=c++17 -I../include dkim_verify_test.cpp ../src/dkim.cpp ../src/dns.cpp -lssl -lcrypto -o /tmp/dkt
#include "look/dkim.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string>
#include <vector>
#include <cstdio>
using namespace look;

static void gen_rsa(std::string& priv, std::string& pub) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    BIO* b1 = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b1, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* p1; long n1 = BIO_get_mem_data(b1, &p1); priv.assign(p1, n1); BIO_free(b1);
    BIO* b2 = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(b2, pkey);
    char* p2; long n2 = BIO_get_mem_data(b2, &p2); pub.assign(p2, n2); BIO_free(b2);
    EVP_PKEY_free(pkey);
}

static int fails = 0;
static void chk(const char* name, bool got, bool want) {
    printf("  %-52s got=%d want=%d  %s\n", name, got, want, got==want?"OK":"FAIL");
    if (got != want) fails++;
}

int main() {
    std::string priv, pub;
    gen_rsa(priv, pub);

    // İmzalayan: d=attacker.com, From: attacker@attacker.com (saldırganın KENDİ geçerli imzası)
    std::vector<DkimHeader> hdrs = {
        {"From", "attacker@attacker.com"},
        {"Subject", "hi"},
    };
    std::string body = "body\r\n";
    std::string sig = dkim_sign(hdrs, body, "attacker.com", "sel", priv);   // "DKIM-Signature: ..."
    std::string signed_from = "From: attacker@attacker.com\r\n";
    std::string subj = "Subject: hi\r\n";

    // --- POZİTİF KONTROL: geçerli imza gerçekten VALID sayılıyor mu (yoksa tüm hüküm anlamsız) ---
    {
        std::string raw = sig + "\r\n" + signed_from + subj + "\r\n" + body;
        chk("POZITIF KONTROL (gecerli imza -> TRUE)", dkim_verify_with_key(raw, pub), true);
    }

    // --- Sanity: tek From ama d= uyumsuz (From: paypal, d=attacker) -> alignment RED ---
    {
        // paypal From'unu imzalamadık → crypto da fail eder; yine de FALSE beklenir
        std::string raw = sig + "\r\n" + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        chk("tek From uyumsuz (paypal, d=attacker) -> FALSE", dkim_verify_with_key(raw, pub), false);
    }

    // --- 3a-A: FAKE From: paypal ÜSTTE, imzalı attacker ALTTA ---
    //   crypto rfind->alt(attacker)->imza uyar->PASS ; alignment find->ust(paypal) vs attacker->?
    {
        std::string raw = sig + "\r\n" + "From: victim@paypal.com\r\n" + signed_from + subj + "\r\n" + body;
        // BYPASS olsaydı TRUE (crypto gecer + alignment yanlis From'a bakip gecer) → GÜVENLİK AÇIĞI
        // Fail-safe ise FALSE (alignment ust paypal'i yakalar). Ölç:
        chk("3a-A fake paypal UST, imzali attacker ALT -> FALSE bekleniyor", dkim_verify_with_key(raw, pub), false);
    }

    // --- 3a-B: imzalı attacker ÜSTTE, FAKE paypal ALTTA ---
    //   crypto rfind->alt(paypal)->imza UYMAZ->FAIL → FALSE beklenir
    {
        std::string raw = sig + "\r\n" + signed_from + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        chk("3a-B imzali attacker UST, fake paypal ALT -> FALSE bekleniyor", dkim_verify_with_key(raw, pub), false);
    }

    printf(fails ? "\n%d FAIL — incele\n" : "\nTUM VAKALAR GECTI (tutarsizlik fail-safe: bypass YOK)\n", fails);
    return fails ? 1 : 0;
}

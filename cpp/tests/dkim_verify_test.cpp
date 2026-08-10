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

    // --- 3b-KRİTİK: h= omission — From İMZALANMAMIŞ (h= sadece Subject) ---
    //   İmza From'u korumuyor → saldırgan From'u değiştirebilir. Alignment yine de mesajdaki
    //   From'u d='ye karşı kontrol ediyor mu (h='den bağımsız)? Ölç:
    {
        std::vector<DkimHeader> h2 = { {"Subject", "hi"} };   // From YOK → h=subject
        std::string sig2 = dkim_sign(h2, body, "attacker.com", "sel", priv);
        // raw'a From: paypal EKLE (imzasız). crypto: subject imzalı→PASS. alignment: d=attacker vs paypal→?
        std::string raw = sig2 + "\r\n" + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        // BYPASS olsaydı TRUE (From imzasız + alignment atlanır/yanlış). Fail-safe ise FALSE.
        chk("3b h= omission (From imzasiz, d=attacker vs From=paypal) -> FALSE bekleniyor",
            dkim_verify_with_key(raw, pub), false);
    }
    // --- 3b: trailing-dot — From: attacker.com. (kök nokta) d=attacker.com, düzgün imzalı ---
    //   Legit trailing-dot From REDDEDİLİRSE false-negative (bypass değil ama uyumluluk). Ölç:
    {
        std::vector<DkimHeader> h3 = { {"From", "u@attacker.com."}, {"Subject", "hi"} };
        std::string sig3 = dkim_sign(h3, body, "attacker.com", "sel", priv);
        std::string raw = sig3 + "\r\n" + "From: u@attacker.com.\r\n" + subj + "\r\n" + body;
        // NOT: bu bir GÜVENLİK testi değil (fail-safe yön); trailing-dot davranışını KAYDEDER.
        bool r = dkim_verify_with_key(raw, pub);
        printf("  %-52s got=%d  (kayit: trailing-dot legit From %s)\n",
               "3b trailing-dot (legit, davranis kaydi)", r, r?"KABUL":"RED-false-negative");
    }

    printf(fails ? "\n%d FAIL — incele\n" : "\nTUM VAKALAR GECTI (② temel+coklu-From+h=omission FAIL-SAFE)\n", fails);
    return fails ? 1 : 0;
}

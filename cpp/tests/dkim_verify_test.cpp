// DKIM verify — positive check + (2) multi-From table (seam: dkim_verify_with_key).
// Hypothesis (to be MEASURED, not ASSUMED): is the inconsistency between alignment find(first From) and crypto rfind(last From)
// exploitable, or is it fail-safe (crypto is bound to the signed one → mismatch → reject)?
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

    // Signer: d=attacker.com, From: attacker@attacker.com (the attacker's OWN valid signature)
    std::vector<DkimHeader> hdrs = {
        {"From", "attacker@attacker.com"},
        {"Subject", "hi"},
    };
    std::string body = "body\r\n";
    std::string sig = dkim_sign(hdrs, body, "attacker.com", "sel", priv);   // "DKIM-Signature: ..."
    std::string signed_from = "From: attacker@attacker.com\r\n";
    std::string subj = "Subject: hi\r\n";

    // --- POSITIVE CHECK: is a valid signature really counted as VALID (otherwise the whole verdict is meaningless) ---
    {
        std::string raw = sig + "\r\n" + signed_from + subj + "\r\n" + body;
        chk("POZITIF KONTROL (gecerli imza -> TRUE)", dkim_verify_with_key(raw, pub), true);
    }

    // --- Sanity: single From but d= mismatched (From: paypal, d=attacker) -> alignment REJECT ---
    {
        // we did not sign paypal's From → crypto fails too; FALSE is still expected
        std::string raw = sig + "\r\n" + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        chk("tek From uyumsuz (paypal, d=attacker) -> FALSE", dkim_verify_with_key(raw, pub), false);
    }

    // --- 3a-A: FAKE From: paypal ON TOP, signed attacker BELOW ---
    //   crypto rfind->bottom(attacker)->signature matches->PASS ; alignment find->top(paypal) vs attacker->?
    {
        std::string raw = sig + "\r\n" + "From: victim@paypal.com\r\n" + signed_from + subj + "\r\n" + body;
        // If it were a BYPASS it would be TRUE (crypto passes + alignment looks at the wrong From and passes) → SECURITY HOLE
        // If fail-safe it is FALSE (alignment catches the top paypal). Measure:
        chk("3a-A fake paypal UST, imzali attacker ALT -> FALSE bekleniyor", dkim_verify_with_key(raw, pub), false);
    }

    // --- 3a-B: signed attacker ON TOP, FAKE paypal BELOW ---
    //   crypto rfind->bottom(paypal)->signature DOES NOT match->FAIL → FALSE expected
    {
        std::string raw = sig + "\r\n" + signed_from + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        chk("3a-B imzali attacker UST, fake paypal ALT -> FALSE bekleniyor", dkim_verify_with_key(raw, pub), false);
    }

    // --- 3b-CRITICAL: h= omission — From NOT SIGNED (h= only Subject) ---
    //   The signature does not protect From → an attacker can change From. Does alignment still check the From
    //   in the message against d= (independently of h=)? Measure:
    {
        std::vector<DkimHeader> h2 = { {"Subject", "hi"} };   // From YOK → h=subject
        std::string sig2 = dkim_sign(h2, body, "attacker.com", "sel", priv);
        // ADD From: paypal to raw (unsigned). crypto: subject signed→PASS. alignment: d=attacker vs paypal→?
        std::string raw = sig2 + "\r\n" + "From: victim@paypal.com\r\n" + subj + "\r\n" + body;
        // If it were a BYPASS it would be TRUE (From unsigned + alignment skipped/wrong). If fail-safe it is FALSE.
        chk("3b h= omission (From imzasiz, d=attacker vs From=paypal) -> FALSE bekleniyor",
            dkim_verify_with_key(raw, pub), false);
    }
    // --- 3b: trailing-dot — From: attacker.com. (root dot) d=attacker.com, properly signed ---
    //   If a legitimate trailing-dot From is REJECTED it is a false negative (not a bypass but a compatibility issue). Measure:
    {
        std::vector<DkimHeader> h3 = { {"From", "u@attacker.com."}, {"Subject", "hi"} };
        std::string sig3 = dkim_sign(h3, body, "attacker.com", "sel", priv);
        std::string raw = sig3 + "\r\n" + "From: u@attacker.com.\r\n" + subj + "\r\n" + body;
        // NOTE: this is not a SECURITY test (fail-safe direction); it RECORDS the trailing-dot behavior.
        bool r = dkim_verify_with_key(raw, pub);
        printf("  %-52s got=%d  (kayit: trailing-dot legit From %s)\n",
               "3b trailing-dot (legit, davranis kaydi)", r, r?"KABUL":"RED-false-negative");
    }

    printf(fails ? "\n%d FAIL — incele\n" : "\nTUM VAKALAR GECTI (② temel+coklu-From+h=omission FAIL-SAFE)\n", fails);
    return fails ? 1 : 0;
}

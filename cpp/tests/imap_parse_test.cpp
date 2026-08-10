// IMAP saf parser tablo testi — look/imap_parse.h (dikişi aç).
// Kilit güvenlik: mailbox adı guard'ı — belgeli geçmiş bug `INBOX; rm -rf /` KABUL'dü, red olmalı.
//   Build (ASan): g++ -std=c++17 -Iinclude -fsanitize=address,undefined tests/imap_parse_test.cpp -o /tmp/ipt
#include "look/imap_parse.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0;
static void eq(const char* what, const std::string& got, const std::string& want) {
    bool ok = got == want;
    printf("    %-22s = %-18s %s\n", what, ("\""+got+"\"").c_str(), ok?"OK":("FAIL want \""+want+"\"").c_str());
    if (!ok) fails++;
}
// mailbox guard: reddedilmeli (false) mi, kabul (true, cleaned) mı?
static void mb(const char* name, bool want_ok, const char* want_clean) {
    std::string cleaned;
    bool ok = imap_mailbox_name_ok(name, cleaned);
    bool pass = (ok == want_ok) && (!want_ok || cleaned == want_clean);
    printf("  mailbox %-26s -> ok=%d clean=%-10s %s\n",
           (std::string("\"")+name+"\"").c_str(), ok, ("\""+cleaned+"\"").c_str(), pass?"OK":"FAIL");
    if (!pass) fails++;
}

int main() {
    printf("IMAP parse_command:\n");
    { std::string t,c,a; imap_parse_command("a1 LOGIN user pass", t,c,a);
      eq("tag",t,"a1"); eq("cmd",c,"LOGIN"); eq("args",a,"user pass"); }
    { std::string t,c,a; imap_parse_command("a2 noop", t,c,a);
      eq("cmd(upper)",c,"NOOP"); eq("args(bos)",a,""); }
    { std::string t,c,a; imap_parse_command("justtag", t,c,a);
      eq("tag-only",t,"justtag"); eq("cmd(bos)",c,""); }

    printf("IMAP split_two:\n");
    { std::string a,b; imap_split_two("\"john\" \"p@ss word\"", a,b);  // tırnaklı, boşluklu pass
      eq("a",a,"john"); eq("b",b,"p@ss word"); }
    { std::string a,b; imap_split_two("bob secret", a,b);
      eq("a",a,"bob"); eq("b",b,"secret"); }

    printf("IMAP mailbox_name_ok (GÜVENLİK — traversal/enjeksiyon):\n");
    mb("INBOX",              true,  "INBOX");
    mb("",                   true,  "INBOX");     // boş → INBOX
    mb("\"INBOX\"",          true,  "INBOX");     // tırnaklı
    mb("Sent",               true,  "Sent");
    mb("\"Drafts\"",         true,  "Drafts");
    mb("INBOX; rm -rf /",    false, "");          // KRİTİK: eskiden KABUL — ';' + boşluk red
    mb("../../etc/passwd",   false, "");          // '..' traversal
    mb("/etc/passwd",        false, "");          // mutlak yol
    mb("a/b",                false, "");          // ayırıcı
    mb("a\\b",               false, "");          // ters ayırıcı
    mb("bad\ttab",           false, "");          // kontrol karakteri

    // --- ADVERSARIAL: ÇÖKME OLMAMALI (ASan) ---
    const char* adv[] = { "", "\"", "\"\"", ";;;;", "\x01\x02", "/", "..", "\\" };
    for (auto s : adv) { std::string c; (void)imap_mailbox_name_ok(s, c);
                         std::string t,cm,a; imap_parse_command(s,t,cm,a);
                         std::string x,y; imap_split_two(s,x,y); }
    std::string big(100000,'"'); { std::string c; (void)imap_mailbox_name_ok(big,c);
                                   std::string x,y; imap_split_two(big,x,y); }
    printf("  adversarial+100K: ÇÖKME YOK\n");

    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (parse + mailbox guard: 'INBOX; rm -rf /' RED, kilitli)\n", fails);
    return fails ? 1 : 0;
}

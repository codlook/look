// SMTP pure-parser table test — smtp_extract_addr (open the seam, look/smtp_parse.h).
// Locks past bugs (bare address, <>, nested <) + there must be no CRASH on adversarial input.
//   Build (ASan): g++ -std=c++17 -Iinclude -fsanitize=address,undefined tests/smtp_parse_test.cpp -o /tmp/spt
#include "look/smtp_parse.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0;
static void chk(const char* in, bool want_ok, const char* want_addr) {
    std::string addr;
    bool ok = smtp_extract_addr(in, addr);
    bool pass = (ok == want_ok) && (!want_ok || addr == want_addr);
    printf("  %-34s -> ok=%d addr=%-14s %s\n", (std::string("\"")+in+"\"").c_str(),
           ok, ("\""+addr+"\"").c_str(), pass ? "OK" : "FAIL");
    if (!pass) fails++;
}

int main() {
    printf("SMTP extract_addr table (past bugs + adversarial):\n");
    // --- legitimate ---
    chk("MAIL FROM:<a@b.com>",        true,  "a@b.com");
    chk("RCPT TO:<x@y.com>",          true,  "x@y.com");
    chk("MAIL FROM:<>",               true,  "");            // null sender (used to be REJECTED)
    chk("MAIL FROM: bare@x.com",      true,  "bare@x.com");  // bare (used to be BROKEN)
    chk("RCPT TO:x@y.com SIZE=100",   true,  "x@y.com");     // bare + ESMTP param → first token
    chk("MAIL FROM:<a@b.com> SIZE=9", true,  "a@b.com");     // angle + param
    // --- must be rejected (malformed) ---
    chk("MAIL FROM",                  false, "");            // no colon
    chk("MAIL FROM:",                 false, "");            // no address
    chk("MAIL FROM:   ",              false, "");            // only whitespace
    chk("MAIL FROM:<a@b<c>",          false, "");            // nested '<'
    chk("MAIL FROM:<a b@c>",          false, "");            // space in the address
    chk("MAIL FROM:<unclosed",        false, "");            // unclosed '<'
    chk("MAIL FROM:bare>evil",        false, "");            // bare + '>' → reject

    // --- ADVERSARIAL: MUST NOT CRASH (caught by ASan) — the return value is irrelevant ---
    const char* adv[] = {
        "", ":", "::::", "<", ">", "<>", ":<", ":<<<<<<<<<<",
        "MAIL FROM:<\x01\x02\x03>", "MAIL FROM:<\xff\xfe>",
        "\r\n\r\n", "MAIL FROM:<a@b.com",
    };
    std::string big(100000, '<'); big = "MAIL FROM:" + big;   // 100K '<' — OOB/quadratic bait
    for (auto s : adv) { std::string a; (void)smtp_extract_addr(s, a); }
    { std::string a; (void)smtp_extract_addr(big, a); }
    printf("  adversarial+100K: NO CRASH (ASan clean)\n");

    printf(fails ? "\n%d FAIL\n" : "\nALL CASES PASSED (extract_addr is robust + past bugs locked in)\n", fails);
    return fails ? 1 : 0;
}

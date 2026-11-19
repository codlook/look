// host[:port] / port parsing table test — look/host_port.h (Go net/url + net.SplitHostPort model).
// No network. Each old behavior is locked by one line: "3306x" → 3306 (stoi stops at the first letter),
// "99999" accepted, "abc" → bare "stoi" / silent default, "[::1]:5432" → host "[".
//   Build: g++ -std=c++17 -Iinclude tests/host_port_test.cpp -o /tmp/hpt && /tmp/hpt
#include "look/host_port.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// input · default port · expected success · expected host · expected port
static void chk(const char* in, int defport, bool wok, const char* whost, int wport) {
    ran++;
    std::string host = "DEF";
    int port = defport;
    std::string err = split_host_port(in, host, port);
    bool ok = err.empty() == wok && (!wok || (host == whost && port == wport));
    printf("  %-22s -> %-5s host=%-10s port=%-5d %s%s\n", (std::string("\"") + in + "\"").c_str(),
           err.empty() ? "ok" : "error", host.c_str(), port, ok ? "OK" : "FAIL",
           err.empty() ? "" : ("   (" + err + ")").c_str());
    if (!ok) fails++;
}
static void num(const char* in, bool wok, int wv) {
    ran++;
    int v = -1;
    bool r = parse_port_number(in, v);
    bool ok = r == wok && (!wok || v == wv);
    printf("  port %-12s -> %s %s\n", (std::string("\"") + in + "\"").c_str(), r ? std::to_string(v).c_str() : "error", ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main() {
    printf("host[:port]:\n");
    chk("db.example",        3306, true,  "db.example", 3306);
    chk("db.example:3307",   3306, true,  "db.example", 3307);
    chk("db.example:",       3306, true,  "db.example", 3306);   // empty port = default (Go net/url)
    chk("",                  3306, true,  "DEF",        3306);   // empty host → the caller's default
    chk(":5433",             5432, true,  "DEF",        5433);
    chk("db.example:3306x",  3306, false, "", 0);                // used to be 3306
    chk("db.example:abc",    3306, false, "", 0);                // used to be a bare "stoi"
    chk("db.example:99999",  3306, false, "", 0);                // used to be accepted
    chk("db.example:0",      3306, false, "", 0);
    chk("db.example:-1",     3306, false, "", 0);
    chk("db.example:+80",    3306, false, "", 0);
    chk("db.example: 80",    3306, false, "", 0);
    chk("db.example:065535", 3306, false, "", 0);                // 6 digits
    chk("db.example:65535",  3306, true,  "db.example", 65535);
    chk("[::1]",             5432, true,  "::1",        5432);
    chk("[::1]:5433",        5432, true,  "::1",        5433);   // used to be host "["
    chk("[fe80::1%eth0]:80", 5432, true,  "fe80::1%eth0", 80);
    chk("[::1]:",            5432, true,  "::1",        5432);
    chk("[::1",              5432, false, "", 0);
    chk("[]:80",             5432, false, "", 0);
    chk("[::1]x",            5432, false, "", 0);
    chk("[::1]:80x",         5432, false, "", 0);
    chk("::1",               5432, false, "", 0);                // IPv6 without brackets (Go: error)
    chk("::1:5432",          5432, false, "", 0);
    printf("port number:\n");
    num("1", true, 1);
    num("65535", true, 65535);
    num("65536", false, 0);
    num("0", false, 0);
    num("", false, 0);
    num("8O80", false, 0);
    num("80 ", false, 0);
    num("99999999999", false, 0);
    printf("%s: %d/%d\n", fails ? "FAIL" : "PASS", ran - fails, ran);
    return fails ? 1 : 0;
}

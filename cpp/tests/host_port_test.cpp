// host[:port] / port ayrıştırma tablo testi — look/host_port.h (Go net/url + net.SplitHostPort modeli).
// Ağsız. Eski davranışların her biri bir satırla kilitli: "3306x" → 3306 (stoi ilk harfte durur),
// "99999" kabul, "abc" → çıplak "stoi" / sessiz varsayılan, "[::1]:5432" → host "[".
//   Build: g++ -std=c++17 -Iinclude tests/host_port_test.cpp -o /tmp/hpt && /tmp/hpt
#include "look/host_port.h"
#include <cstdio>
#include <string>
using namespace look;

static int fails = 0, ran = 0;
// girdi · varsayılan port · beklenen başarı · beklenen host · beklenen port
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
    chk("db.example:",       3306, true,  "db.example", 3306);   // boş port = varsayılan (Go net/url)
    chk("",                  3306, true,  "DEF",        3306);   // boş host → çağıranın varsayılanı
    chk(":5433",             5432, true,  "DEF",        5433);
    chk("db.example:3306x",  3306, false, "", 0);                // eskiden 3306
    chk("db.example:abc",    3306, false, "", 0);                // eskiden çıplak "stoi"
    chk("db.example:99999",  3306, false, "", 0);                // eskiden kabul
    chk("db.example:0",      3306, false, "", 0);
    chk("db.example:-1",     3306, false, "", 0);
    chk("db.example:+80",    3306, false, "", 0);
    chk("db.example: 80",    3306, false, "", 0);
    chk("db.example:065535", 3306, false, "", 0);                // 6 hane
    chk("db.example:65535",  3306, true,  "db.example", 65535);
    chk("[::1]",             5432, true,  "::1",        5432);
    chk("[::1]:5433",        5432, true,  "::1",        5433);   // eskiden host "["
    chk("[fe80::1%eth0]:80", 5432, true,  "fe80::1%eth0", 80);
    chk("[::1]:",            5432, true,  "::1",        5432);
    chk("[::1",              5432, false, "", 0);
    chk("[]:80",             5432, false, "", 0);
    chk("[::1]x",            5432, false, "", 0);
    chk("[::1]:80x",         5432, false, "", 0);
    chk("::1",               5432, false, "", 0);                // parantezsiz IPv6 (Go: hata)
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

// resp_tls_harness.cpp — Redis TLS (rediss://) canlı smoke-test.
// RespClient'ı doğrudan kullanır: rediss:// bağlan → SET/GET → değer parite mi.
// tls=verify ayrı test edilir (self-signed → reddedilmeli). Docker redis:7 + TLS gerektirir.
#include "look/resp_client.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <redis-url>\n", argv[0]); return 2; }
    try {
        look::RespClient r(argv[1]);
        r.set("tls_probe", "hello-tls", 60);
        std::string v = r.get("tls_probe");
        printf("OK set/get over: %s -> [%s]\n", argv[1], v.c_str());
        return v == "hello-tls" ? 0 : 1;
    } catch (const std::exception& e) {
        printf("THROW: %s\n", e.what());
        return 3;
    }
}

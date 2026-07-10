// IMAP M1 test harness — ImapServer'ı dummy auth ile başlatır.
// Kullanım: imap-m1-test [port]   (default 7440)
// Auth: user="test" pass="sifre" kabul, gerisi ret.
#include "look/imap_server.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    int port  = (argc > 1) ? std::atoi(argv[1]) : 7440;
    int ports = (argc > 2) ? std::atoi(argv[2]) : 0;    // IMAPS (implicit TLS) portu
    look::ImapAuthHandler auth = [](const std::string& u, const std::string& p) {
        look::ImapAuthResult r;
        if (u == "test" && p == "sifre") { r.ok = true; r.maildir_path = "/tmp/maildir/test"; }
        return r;
    };
    look::ImapServer srv(port, ports, 4, auth);
    srv.start();
    std::printf("IMAP M1 test — port %d (test/sifre)\n", port);
    // 60 sn çalış, sonra çık (test otomasyonu için)
    std::this_thread::sleep_for(std::chrono::seconds(60));
    srv.stop();
    return 0;
}

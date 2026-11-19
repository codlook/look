// IMAP M1 test harness — starts ImapServer with a dummy auth.
// Usage: imap-m1-test [port]   (default 7440)
// Auth: user="test" pass="sifre" accepted, everything else rejected.
#include "look/imap_server.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    int port  = (argc > 1) ? std::atoi(argv[1]) : 7440;
    int ports = (argc > 2) ? std::atoi(argv[2]) : 0;    // IMAPS (implicit TLS) port
    look::ImapAuthHandler auth = [](const std::string& u, const std::string& p) {
        look::ImapAuthResult r;
        if (u == "test" && p == "sifre") { r.ok = true; r.maildir_path = "/tmp/maildir/test"; }
        return r;
    };
    look::ImapServer srv(port, ports, 4, auth);
    srv.start();
    std::printf("IMAP M1 test — port %d (test/sifre)\n", port);
    // run for 60 s, then exit (for test automation)
    std::this_thread::sleep_for(std::chrono::seconds(60));
    srv.stop();
    return 0;
}

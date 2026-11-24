// EpollEventLoop::async_write queues writes behind each other; it never replaces one that is still on its way.
//
// OLD: one pending write per descriptor — a second async_write while the first was not out (the peer had not taken
// it all) REPLACED it: the first's bytes were never sent and its callback was never called. SMTP over TLS 1.3 lost
// its session tickets that way ("bad record mac" at the client, 3-6% of the sessions on a loaded machine).
//
// The test gives the connection a tiny send buffer and a peer that does not read, writes 600 KB, then 4 bytes, then
// 5 MiB, and checks:
//   1) the 4 bytes arrive AFTER the 600 KB, nothing is lost, nothing is reordered,
//   2) the callbacks of the first two writes run in the order of the writes, both with ok=true,
//   3) the third write (more than the buffer limit allows) is refused at once with ok=false and none of its bytes arrive,
//   4) the connection still works afterwards.
// Linux only (epoll). Compile: g++ -std=c++17 -Iinclude tests/event_loop_write_queue_test.cpp src/event_loop.cpp -lpthread
#include "look/event_loop.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace look;
static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("  OK   %s\n", msg); else { std::printf("  FAIL %s\n", msg); g_fail = 1; } } while (0)

int main() {
#ifndef __linux__
    std::printf("  (skipped: epoll is Linux only)\n"); return 0;
#else
    auto loop = EventLoop::create();

    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (::bind(srv, (sockaddr*)&a, sizeof a) != 0 || ::listen(srv, 4) != 0) { std::printf("  FAIL cannot listen\n"); return 1; }
    socklen_t al = sizeof a; ::getsockname(srv, (sockaddr*)&a, &al);

    std::mutex m; std::condition_variable cv; int accepted = -1;
    loop->listen(srv, [&](int cfd) {
        int sb = 4096; ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);   // a tiny send buffer: the write stays pending
        std::lock_guard<std::mutex> lk(m); accepted = cfd; cv.notify_all();
    });
    std::thread lt([&] { loop->run(); });

    int peer = ::socket(AF_INET, SOCK_STREAM, 0);
    int rb = 4096; ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    if (::connect(peer, (sockaddr*)&a, sizeof a) != 0) { std::printf("  FAIL cannot connect\n"); return 1; }
    int fd;
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(5), [&] { return accepted >= 0; }); fd = accepted; }
    if (fd < 0) { std::printf("  FAIL no connection accepted\n"); return 1; }

    const size_t A = 600 * 1024;
    std::vector<std::string> order; std::vector<bool> oks;
    auto done = [&](const char* name) { return [&, name](bool ok) { std::lock_guard<std::mutex> lk(m); order.push_back(name); oks.push_back(ok); cv.notify_all(); }; };
    // all three on the loop thread, back to back, while the peer reads nothing
    loop->post([&] {
        loop->async_write(fd, std::string(A, 'a'), done("A"));
        loop->async_write(fd, std::string("BBBB"), done("B"));
        loop->async_write(fd, std::string(size_t(5) << 20, 'c'), done("C"));
    });
    // C is refused at once
    std::vector<std::string> ord; std::vector<bool> okv;   // copies, taken under the lock: the callbacks keep writing
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(5), [&] { return !order.empty(); }); ord = order; okv = oks; }
    CHECK(ord.size() == 1 && ord[0] == "C" && !okv[0], "a write over the buffer limit is refused at once (ok=false)");

    // now the peer reads everything: 600 KB of 'a' and then BBBB, nothing else
    std::string got; char buf[65536];
    ::timeval tv{5, 0}; ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got.size() < A + 4) { ssize_t r = ::recv(peer, buf, sizeof buf, 0); if (r <= 0) break; got.append(buf, (size_t)r); }
    CHECK(got.size() == A + 4, "all 600 KB + 4 bytes arrive (nothing lost)");
    bool a_ok = got.size() >= A && got.compare(0, A, std::string(A, 'a')) == 0;
    CHECK(a_ok, "the first write arrives whole and first");
    CHECK(got.size() == A + 4 && got.compare(A, 4, "BBBB") == 0, "the second write arrives after the first, in order");
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(5), [&] { return order.size() >= 3; }); ord = order; okv = oks; }
    CHECK(ord.size() == 3 && ord[1] == "A" && ord[2] == "B" && okv[1] && okv[2], "the callbacks of the two queued writes run in order, ok=true");

    // the connection still works, and none of the refused bytes arrive
    loop->post([&] { loop->async_write(fd, std::string("DONE"), done("D")); });
    std::string tail;
    while (tail.size() < 4) { ssize_t r = ::recv(peer, buf, sizeof buf, 0); if (r <= 0) break; tail.append(buf, (size_t)r); }
    CHECK(tail == "DONE", "a later write still goes out, and nothing of the refused write came before it");

    loop->stop();
    ::shutdown(srv, SHUT_RDWR); ::close(peer);
    // wake the loop so run() returns
    int wake = ::socket(AF_INET, SOCK_STREAM, 0); ::connect(wake, (sockaddr*)&a, sizeof a); ::close(wake);
    lt.join();
    std::printf(g_fail ? "FAIL: event loop write queue\n" : "PASS: event loop write queue\n");
    return g_fail;
#endif
}

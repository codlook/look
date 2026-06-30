#include "look/sse.h"
#include <cstdlib>
#include <string>
#include <stdexcept>

#if defined(__linux__)
#  include <sys/socket.h>
#  include <unistd.h>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#elif defined(_WIN32)
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
#  define MSG_NOSIGNAL 0
#endif

namespace look {

SseRegistry g_sse_registry;

bool SseConnection::send(const std::string& data, const std::string& event_name) {
    if (closed.load()) return false;

    std::string frame;
    if (!event_name.empty())
        frame = "event: " + event_name + "\ndata: " + data + "\n\n";
    else
        frame = "data: " + data + "\n\n";

    std::lock_guard<std::mutex> lk(write_mutex);
    if (closed.load()) return false;
#if defined(_WIN32)
    int sent = ::send((SOCKET)fd, frame.data(), (int)frame.size(), 0);
#else
    int sent = (int)::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
#endif
    if (sent <= 0) {
        closed.store(true);
        return false;
    }
    return true;
}

bool SseConnection::send_comment(const std::string& comment) {
    if (closed.load()) return false;
    std::string frame = ": " + comment + "\n\n";
    std::lock_guard<std::mutex> lk(write_mutex);
    if (closed.load()) return false;
#if defined(_WIN32)
    int sent = ::send((SOCKET)fd, frame.data(), (int)frame.size(), 0);
#else
    int sent = (int)::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
#endif
    return sent > 0;
}

void SseConnection::close_conn() {
    if (closed.exchange(true)) return;  // already closed
    g_sse_registry.remove(fd);
    if (on_close_cb) on_close_cb();
}

void SseRegistry::add(std::shared_ptr<SseConnection> conn) {
    // LOOK_SSE_MAX_CONN env ile yapılandırılabilir (varsayılan: 1024)
    static const size_t MAX_SSE = []() -> size_t {
        const char* e = std::getenv("LOOK_SSE_MAX_CONN");
        return (e && *e) ? (size_t)std::stoul(e) : 1024;
    }();
    std::unique_lock<std::shared_mutex> lk(mutex_);
    if (clients_.size() >= MAX_SSE)
        throw std::runtime_error("SSE bağlantı limiti aşıldı (" + std::to_string(MAX_SSE) + ")");
    clients_[conn->fd] = std::move(conn);
}

void SseRegistry::remove(int fd) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    clients_.erase(fd);
}

size_t SseRegistry::count() {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return clients_.size();
}

} // namespace look

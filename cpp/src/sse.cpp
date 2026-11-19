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

// SSE event injection defense. In SSE, \n ends a line and \n\n is the event separator; raw
// user data can inject a fake event/id/data field (the SSE counterpart of CRLF header
// injection).
//   * Single-line fields (event, id, comment): \r and \n are stripped completely.
//   * data: may be multi-line -- per the SSE spec EVERY line is sent with its own "data: "
//     prefix. This way a line break in the content cannot create a fake field; every
//     line becomes data content (legitimate multi-line data is preserved).
static std::string sse_strip_nl(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) if (c != '\r' && c != '\n') out += c;
    return out;
}
static std::string sse_encode_data(const std::string& data) {
    // Normalize line endings (\r\n, \r, \n -> \n), then prefix every line with "data: ".
    std::string norm; norm.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\r') { norm += '\n'; if (i + 1 < data.size() && data[i+1] == '\n') ++i; }
        else norm += data[i];
    }
    std::string out; size_t start = 0;
    while (true) {
        size_t nl = norm.find('\n', start);
        out += "data: " + norm.substr(start, (nl == std::string::npos ? norm.size() : nl) - start) + "\n";
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

bool SseConnection::send(const std::string& data, const std::string& event_name) {
    if (closed.load()) return false;

    std::string frame;
    if (!event_name.empty())
        frame = "event: " + sse_strip_nl(event_name) + "\n";
    frame += sse_encode_data(data);   // multi-line data -> "data: " per line
    frame += "\n";                    // blank line that ends the event

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
    std::string frame = ": " + sse_strip_nl(comment) + "\n\n";   // \r\n stripped -- the stream is not corrupted
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
    if (closed.exchange(true)) return;  // already closed (send-gate + idempotent)
    // Have the loop thread do the FULL cleanup (fd close + sse_clients erase + epoll DEL +
    // on_close_cb). Previously only closed+registry+cb were done -> fd/epoll/map LEAKED until the client's
    // TCP dropped (measured: after sse::close the fd base+1 stayed). Also, since the
    // socket was not closed the client did not know the stream had ended. request_loop_close
    // hands close_sse over to the loop thread with loop->post (own-once -> no double close).
    if (request_loop_close) {
        request_loop_close();
    } else {
        // No loop (CLI/test context): at least the registry + cb.
        g_sse_registry.remove(fd);
        if (auto cb = get_on_close_cb()) cb();
    }
}

// There is no limit on the number of open connections: the ceiling is the operating system's file limit;
// the per-address limit is in http_server.
void SseRegistry::add(std::shared_ptr<SseConnection> conn) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
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

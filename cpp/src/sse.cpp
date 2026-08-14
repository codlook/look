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

// SSE event injection savunması. SSE'de \n satır, \n\n event ayırıcıdır; ham
// kullanıcı verisi sahte event/id/data alanı enjekte edebilir (CRLF header
// injection'ın SSE karşılığı).
//   • Tek-satırlık alanlar (event, id, comment): \r ve \n tamamen sıyrılır.
//   • data: çok-satırlı olabilir — SSE spec'ine göre HER satır ayrı "data: "
//     ile gönderilir. Böylece içerikteki satır-sonu sahte alan yaratamaz; her
//     satır zorunlu olarak data içeriği olur (meşru çok-satırlı data korunur).
static std::string sse_strip_nl(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) if (c != '\r' && c != '\n') out += c;
    return out;
}
static std::string sse_encode_data(const std::string& data) {
    // Satır-sonlarını normalize et (\r\n, \r, \n → \n), sonra her satır "data: ".
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
    frame += sse_encode_data(data);   // çok-satırlı data → satır-başına "data: "
    frame += "\n";                    // event'i bitiren boş satır

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
    std::string frame = ": " + sse_strip_nl(comment) + "\n\n";   // \r\n sıyrılır — stream bozulmaz
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
    // Loop thread'inde TAM temizlik yaptır (fd close + sse_clients erase + epoll DEL +
    // on_close_cb). Eskiden yalnız closed+registry+cb yapılıyordu → fd/epoll/map, istemci
    // TCP-kopana kadar SIZIYORDU (ölçüldü: sse::close sonrası fd base+1 kalıyor). Ayrıca
    // soket kapanmadığı için istemci akışın bittiğini bilmiyordu. request_loop_close,
    // close_sse'yi loop->post ile loop thread'ine devreder (own-once → çift-close yok).
    if (request_loop_close) {
        request_loop_close();
    } else {
        // Loop yok (CLI/test bağlamı): en azından registry + cb.
        g_sse_registry.remove(fd);
        if (on_close_cb) on_close_cb();
    }
}

void SseRegistry::add(std::shared_ptr<SseConnection> conn) {
    // LOOK_SSE_MAX_CONN env ile yapılandırılabilir (varsayılan: 1024)
    static const size_t MAX_SSE = []() -> size_t {
        const char* e = std::getenv("LOOK_SSE_MAX_CONN");
        return (e && *e) ? (size_t)std::stoul(e) : 1024;
    }();
    std::unique_lock<std::shared_mutex> lk(mutex_);
    if (clients_.size() >= MAX_SSE)
        throw std::runtime_error("SSE connection limit exceeded (" + std::to_string(MAX_SSE) + ")");
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

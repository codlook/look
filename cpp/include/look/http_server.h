#pragma once

#include <string>
#include <map>
#include <functional>
#include <memory>
#include "look/event_loop.h"
#include "look/websocket.h"
#include "look/sse.h"

namespace look {

// ── Parsed HTTP request (minimal — only what dispatch needs) ──────────────────

struct HttpRequest {
    std::string method;          // GET POST PUT DELETE PATCH
    std::string path;            // /api/users
    std::string query_string;    // id=1&sort=asc
    std::string version;         // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;
    bool upgrade_websocket = false;  // Upgrade: websocket header present
    std::string ws_key;              // Sec-WebSocket-Key value
    bool upgrade_sse = false;        // Accept: text/event-stream
};

// ── HTTP response builder ─────────────────────────────────────────────────────

struct HttpResponse {
    int status_code       = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
    bool keep_alive = false;

    std::string build() const;
};

// ── Per-connection state ──────────────────────────────────────────────────────

enum class ConnState { READING_HEADERS, READING_BODY, DISPATCHING, DONE };

struct HttpConn {
    int fd = -1;
    ConnState state = ConnState::READING_HEADERS;
    std::string buf;
    HttpRequest req;
    bool keep_alive = false;
};

// ── Request handler signature ─────────────────────────────────────────────────

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;
using WsHandler   = std::function<void(std::shared_ptr<WsConnection>,  const HttpRequest&)>;
using SseHandler  = std::function<void(std::shared_ptr<SseConnection>, const HttpRequest&)>;

// ── HTTP server ───────────────────────────────────────────────────────────────
//
// Owns an EventLoop. Call listen() then run().
// Each accepted connection is parsed as HTTP/1.1.
// Completed requests are dispatched to the handler on a worker thread,
// response is written back on the event loop thread.

class HttpServer {
public:
    explicit HttpServer(int port, int workers, HttpHandler handler,
                        WsHandler  ws_handler  = nullptr,
                        SseHandler sse_handler = nullptr);
    ~HttpServer();

    // Start listening and enter event loop — blocks.
    void run();

    // Stop the server.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace look

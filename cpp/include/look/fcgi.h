#pragma once

// FastCGI 1.0 wire protocol -- written from scratch with minimal external runtime dependencies.
// Spec: https://fastcgi-archives.github.io/FastCGI_Specification.html
//
// Two modes:
//   1. stdin/stdout (mod_fcgid):
//      FcgiServer server;
//
//   2. TCP (mod_proxy_fcgi, PHP-FPM model):
//      FcgiServer server(9000);   // listen on port 9000
//      server.run(handler);       // call handler for each connection
//
// The .lk code does not change in either mode.

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <functional>

#ifdef _WIN32
  // winsock2.h must come BEFORE windows.h
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

namespace look {

// ── FCGI record types ─────────────────────────────────────────────────────────
enum FcgiType : uint8_t {
    FCGI_BEGIN_REQUEST     = 1,
    FCGI_ABORT_REQUEST     = 2,
    FCGI_END_REQUEST       = 3,
    FCGI_PARAMS            = 4,
    FCGI_STDIN             = 5,
    FCGI_STDOUT            = 6,
    FCGI_STDERR            = 7,
    FCGI_DATA              = 8,
    FCGI_GET_VALUES        = 9,
    FCGI_GET_VALUES_RESULT = 10,
};

enum FcgiRole : uint16_t {
    FCGI_RESPONDER  = 1,
    FCGI_AUTHORIZER = 2,
    FCGI_FILTER     = 3,
};

enum FcgiProtoStatus : uint8_t {
    FCGI_REQUEST_COMPLETE = 0,
    FCGI_CANT_MPX_CONN    = 1,
    FCGI_OVERLOADED       = 2,
    FCGI_UNKNOWN_ROLE     = 3,
};

// ── Parsed request ────────────────────────────────────────────────────────────
struct FcgiRequest {
    uint16_t id       = 0;
    bool     keep_conn = false;
    std::map<std::string, std::string> params; // CGI env vars
    std::string body;
};

// ── FcgiConn -- I/O on a single connection ────────────────────────────────
// In TCP mode every accepted socket becomes an FcgiConn.
// In stdin/stdout mode a single FcgiConn is used for the whole process lifetime.
class FcgiConn {
public:
    // stdin/stdout mode
    FcgiConn();

    // TCP mode -- accepted socket
#ifdef _WIN32
    explicit FcgiConn(SOCKET sock);
#else
    explicit FcgiConn(int sock);
#endif

    ~FcgiConn();

    FcgiConn(const FcgiConn&)            = delete;
    FcgiConn& operator=(const FcgiConn&) = delete;
    FcgiConn(FcgiConn&&) noexcept;
    FcgiConn& operator=(FcgiConn&&) noexcept;

    // Read the next request. false = connection closed.
    bool accept(FcgiRequest& req);

    // Keep-alive idle timeout (seconds) -- SO_RCVTIMEO in TCP mode.
    // While waiting for the next request on a KEEP_CONN connection it prevents the worker thread from
    // being pinned indefinitely: timeout -> accept() false -> the connection closes,
    // the thread returns to the pool. Apache transparently reopens a dropped connection.
    void set_read_timeout(int seconds);

    // Write response
    void write_stdout(uint16_t req_id, const std::string& data);
    void write_stderr(uint16_t req_id, const std::string& data);
    void end_request(uint16_t req_id, uint32_t app_status = 0);

private:
    bool raw_read (uint8_t* buf, size_t len);
    bool raw_write(const uint8_t* buf, size_t len);
    bool read_record(uint8_t& type, uint16_t& req_id, std::vector<uint8_t>& content);
    void write_record(uint8_t type, uint16_t req_id, const uint8_t* data, size_t len);
    void parse_params(const uint8_t* p, size_t len,
                      std::map<std::string, std::string>& out);
    uint32_t read_len(const uint8_t*& p, const uint8_t* end);

    bool   tcp_mode_ = false;
#ifdef _WIN32
    HANDLE h_in_    = INVALID_HANDLE_VALUE;
    HANDLE h_out_   = INVALID_HANDLE_VALUE;
    SOCKET tcp_sock_ = INVALID_SOCKET;
#else
    int fd_in_   = STDIN_FILENO;
    int fd_out_  = STDOUT_FILENO;
    int tcp_sock_ = -1;
#endif
};

// ── FcgiServer -- TCP listener (PHP-FPM model) ────────────────────────────────
// Usage:
//   FcgiServer srv(9000);
//   srv.run([](FcgiConn& conn) {
//       FcgiRequest req;
//       while (conn.accept(req)) {
//           conn.write_stdout(req.id, "Status: 200 OK\r\n\r\nOK");
//           conn.end_request(req.id, 0);
//       }
//   });
class FcgiServer {
public:
    explicit FcgiServer(int port);
    ~FcgiServer();

    // Accept connections continuously, call handler for each connection.
    // handler(conn) -- by value, so conn can be handed to the thread pool.
    using Handler = std::function<void(FcgiConn)>;
    void run(Handler handler);

    // Graceful shutdown: new connection acceptance stops, run() returns (look/shutdown.h). May be called from any
    // thread.
    void stop();

private:
#ifdef _WIN32
    SOCKET listen_sock_ = INVALID_SOCKET;
#else
    int listen_sock_ = -1;
#endif
    int port_;
};

// ── Backward compatibility: the old FcgiServer API (stdin/stdout) ─────────────
// Existing code in fcgi_main.cpp works unchanged.
// New TCP code uses FcgiServer(port) + FcgiConn.
//
// Old usage:
//   FcgiServer server;
//   FcgiRequest req;
//   while (server.accept(req)) { ... }
//
// This alias wraps FcgiConn -- no change needed.
class FcgiServerLegacy {
public:
    FcgiServerLegacy() : conn_() {}
    bool accept(FcgiRequest& req)                    { return conn_.accept(req); }
    void write_stdout(uint16_t id, const std::string& d) { conn_.write_stdout(id, d); }
    void write_stderr(uint16_t id, const std::string& d) { conn_.write_stderr(id, d); }
    void end_request(uint16_t id, uint32_t s = 0)    { conn_.end_request(id, s); }
private:
    FcgiConn conn_;
};

} // namespace look

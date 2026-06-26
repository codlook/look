#pragma once

// FastCGI 1.0 wire protocol — sifir bagimlilik, sifirdan yazildi.
// Spec: https://fastcgi-archives.github.io/FastCGI_Specification.html
//
// Iki mod:
//   1. stdin/stdout (mod_fcgid):
//      FcgiServer server;
//
//   2. TCP (mod_proxy_fcgi, PHP-FPM modeli):
//      FcgiServer server(9000);   // port 9000'de dinle
//      server.run(handler);       // her baglanti icin handler cagir
//
// Her iki modda da .lk kodu degismez.

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <functional>

#ifdef _WIN32
  // winsock2.h windows.h'dan ONCE gelmeli
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

// ── FcgiConn — tek bir baglanti uzerindeki I/O ────────────────────────────────
// TCP modunda her kabul edilen socket bir FcgiConn olur.
// stdin/stdout modunda process omru boyunca tek bir FcgiConn kullanilir.
class FcgiConn {
public:
    // stdin/stdout modu
    FcgiConn();

    // TCP modu — kabul edilmis socket
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

    // Bir sonraki istegi oku. false = baglanti kapandi.
    bool accept(FcgiRequest& req);

    // Response yaz
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

// ── FcgiServer — TCP listener (PHP-FPM modeli) ────────────────────────────────
// Kullanim:
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

    // Surekli baglanti kabul et, her baglanti icin handler cagir.
    // handler(conn) — conn'u thread pool'a devretmek icin by-value.
    using Handler = std::function<void(FcgiConn)>;
    void run(Handler handler);

private:
#ifdef _WIN32
    SOCKET listen_sock_ = INVALID_SOCKET;
#else
    int listen_sock_ = -1;
#endif
    int port_;
};

// ── Geriye donuk uyumluluk: eski FcgiServer API'si (stdin/stdout) ─────────────
// fcgi_main.cpp'deki mevcut kod degismeden calisir.
// Yeni TCP kodu FcgiServer(port) + FcgiConn kullanir.
//
// Eski kullanim:
//   FcgiServer server;
//   FcgiRequest req;
//   while (server.accept(req)) { ... }
//
// Bu alias FcgiConn'u wrap eder — degisiklik gerekmez.
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

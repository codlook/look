#pragma once

#include "look/db_connection.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <chrono>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static const sock_t SOCK_INVALID = INVALID_SOCKET;
  inline void close_sock(sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  using sock_t = int;
  static const sock_t SOCK_INVALID = -1;
  inline void close_sock(sock_t s) { ::close(s); }
#endif

namespace look {

struct DbConfig {
    std::string host     = "127.0.0.1";
    int         port     = 3306;
    std::string user;
    std::string password;
    std::string database;

    int connect_timeout_ms = 5000;
    int query_timeout_ms   = 30000;
    int max_reconnect      = 3;
    int reconnect_delay_ms = 500;
};

class MySQLClient : public DbConnection {
public:
    MySQLClient();
    ~MySQLClient() override;

    void connect(const std::string& host, int port,
                 const std::string& user, const std::string& password,
                 const std::string& database);
    void connect(const DbConfig& cfg);
    void disconnect();

    // DbConnection arayüzü
    std::vector<DbRow> query(const std::string& sql) override;
    std::vector<DbRow> execute(const std::string& sql, const std::vector<DbParam>& params) override;
    int64_t last_insert_id() const override { return (int64_t)last_insert_id_; }
    int64_t affected_rows()  const override { return (int64_t)affected_rows_;  }
    void    close()          override       { disconnect(); }
    bool    is_connected()   const override { return sock_ != SOCK_INVALID; }
    const char* driver_name() const override { return "mysql"; }

    // MySQL'e özgü — arayüzde yok
    bool    ping();
    int64_t last_query_ms() const { return last_query_ms_; }

    static std::string escape(const std::string& s);
    const DbConfig& config() const { return cfg_; }

private:
    sock_t   sock_           = SOCK_INVALID;
    DbConfig cfg_;
    uint64_t last_insert_id_ = 0;
    uint64_t affected_rows_  = 0;
    int64_t  last_query_ms_  = 0;
    bool     wsock_init_     = false;

    struct StmtMeta { uint32_t id; uint16_t cols; uint16_t params; };
    StmtMeta             stmt_prepare(const std::string& sql);
    std::vector<DbRow>   stmt_execute(const StmtMeta& m, const std::vector<DbParam>& params);
    void                 stmt_close(uint32_t stmt_id);

    void ensure_connected();
    void do_connect();
    void set_socket_timeout(int ms);

    std::vector<uint8_t> read_packet(uint8_t& seq);
    void send_packet(const std::vector<uint8_t>& data, uint8_t seq);
    void send_bytes(const uint8_t* buf, size_t len);
    bool recv_bytes(uint8_t* buf, size_t len);

    void do_handshake(const std::string& user,
                      const std::string& password,
                      const std::string& database);

    static std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> native_password(const std::string& password,
                                                  const std::string& challenge);

    static uint64_t    read_lenenc(const uint8_t*& p, const uint8_t* end);
    static std::string read_lenenc_str(const uint8_t*& p, const uint8_t* end);
    static uint16_t    read_u16(const uint8_t*& p);
    static uint32_t    read_u32(const uint8_t*& p);
    static uint64_t    read_u64(const uint8_t*& p);
};

} // namespace look

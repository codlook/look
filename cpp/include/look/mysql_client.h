#pragma once

#include "look/db_connection.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <chrono>
#include <memory>
#include <unordered_map>

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
    bool        tls = false;        // mysqls:// or ?tls=1 → TLS (SSLRequest + SSL_connect)
    bool        tls_verify = false;  // ?tls=verify → SSL_VERIFY_PEER + hostname (against MITM).
                                     // tls && !tls_verify = --ssl-mode=REQUIRED (encrypt, no verification,
                                     // self-signed friendly); tls_verify = --ssl-mode=VERIFY_IDENTITY.

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

    // DbConnection interface
    std::vector<DbRow> query(const std::string& sql) override;
    std::vector<DbRow> execute(const std::string& sql, const std::vector<DbParam>& params) override;
    int64_t last_insert_id() const override { return (int64_t)last_insert_id_; }
    int64_t affected_rows()  const override { return (int64_t)affected_rows_;  }
    void    close()          override       { disconnect(); }
    bool    is_connected()   const override { return sock_ != SOCK_INVALID; }
    const char* driver_name() const override { return "mysql"; }
    bool in_transaction() const override { return in_txn_ || tx_depth > 0; }

    // MySQL-specific — not in the interface
    bool    ping();
    int64_t last_query_ms() const { return last_query_ms_; }

    static std::string escape(const std::string& s);
    const DbConfig& config() const { return cfg_; }

private:
    sock_t   sock_           = SOCK_INVALID;
    void*    ssl_            = nullptr;  // SSL* (POSIX/OpenSSL); when TLS is active send/recv_bytes go through here
    bool     quitting_       = false;    // while disconnect sends COM_QUIT (so that a send error does not call disconnect again)
    DbConfig cfg_;
    uint64_t last_insert_id_ = 0;
    uint64_t affected_rows_  = 0;
    // SERVER_STATUS_IN_TRANS in the server's last OK/EOF packet: are we inside a transaction.
    // If a dropped connection was inside a transaction a new connection is NOT OPENED (see ensure_connected).
    bool     in_txn_         = false;
    int64_t  last_query_ms_  = 0;
    bool     wsock_init_     = false;

    struct StmtMeta { uint32_t id; uint16_t cols; uint16_t params; };
    StmtMeta             stmt_prepare(const std::string& sql);
    std::vector<DbRow>   stmt_execute(const StmtMeta& m, const std::vector<DbParam>& params);
    void                 stmt_close(uint32_t stmt_id);
    uint16_t             last_error_code_ = 0;   // code of the last ERR packet (to tell 1243 from 1461)

    // Prepared-statement cache (per connection). The same SQL is not prepared a second time: one round trip
    // per query (it used to be PREPARE + EXECUTE + CLOSE = two). Emptied when the connection drops
    // (statement ids belong to that session); the upper bound protects the server's total statement limit.
    struct CachedStmt { StmtMeta meta; uint64_t used; };
    std::unordered_map<std::string, CachedStmt> stmt_cache_;
    uint64_t stmt_tick_ = 0;
    static constexpr size_t STMT_CACHE_MAX = 64;
    void clear_stmt_cache(bool close_on_server);
    // An error packet arriving between rows and CALL's extra result sets (SERVER_MORE_RESULTS_EXISTS)
    [[noreturn]] void throw_err_packet(const std::vector<uint8_t>& pkt, const char* prefix);
    void drain_more_results(uint16_t status, uint8_t& seq);

    // Read buffer: several packets per recv (it used to be header + body per packet = two recvs).
    std::unique_ptr<uint8_t[]> rbuf_;
    size_t rbuf_pos_ = 0, rbuf_len_ = 0;
    static constexpr size_t RBUF_SIZE = 16384;
    int  recv_some(uint8_t* buf, size_t cap);   // plain TCP: at least 1 byte; <=0 → connection ended

    void ensure_connected();
    void do_connect();
    void set_socket_timeout(int ms);
    int  rcv_tmo_ms_ = -1;   // last set_socket_timeout value (for the fiber wait)

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

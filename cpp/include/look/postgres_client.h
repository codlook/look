#pragma once

#include "look/db_connection.h"
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using pg_sock_t = SOCKET;
  static const pg_sock_t PG_SOCK_INVALID = INVALID_SOCKET;
  inline void pg_close_sock(pg_sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  using pg_sock_t = int;
  static const pg_sock_t PG_SOCK_INVALID = -1;
  inline void pg_close_sock(pg_sock_t s) { ::close(s); }
#endif

namespace look {

// PostgreSQL type code sabitleri (MySQL/SQLite ile çakışmaz)
namespace pg_type {
    static constexpr uint8_t INT   = 0x30;
    static constexpr uint8_t FLOAT = 0x31;
    static constexpr uint8_t BOOL  = 0x32;
    static constexpr uint8_t NUL   = 0x33;
}

class PostgresClient : public DbConnection {
public:
    PostgresClient();
    ~PostgresClient() override;

    void connect(const std::string& host, int port,
                 const std::string& user, const std::string& password,
                 const std::string& database);
    void disconnect();

    // DbConnection arayüzü
    std::vector<DbRow> query(const std::string& sql) override;
    std::vector<DbRow> execute(const std::string& sql, const std::vector<DbParam>& params) override;
    int64_t last_insert_id() const override { return last_insert_id_; }
    int64_t affected_rows()  const override { return affected_rows_; }
    void    close()          override       { disconnect(); }
    bool    is_connected()   const override { return sock_ != PG_SOCK_INVALID; }
    const char* driver_name() const override { return "postgres"; }

private:
    pg_sock_t   sock_               = PG_SOCK_INVALID;
    std::string host_;
    int         port_               = 5432;
    std::string user_;
    std::string password_;
    std::string database_;
    int64_t     last_insert_id_     = 0;
    int64_t     affected_rows_      = 0;
    bool        wsock_init_         = false;
    int         connect_timeout_ms_ = 5000;
    int         query_timeout_ms_   = 30000;

    void do_connect();
    void set_socket_timeout(int ms);
    bool recv_bytes(uint8_t* buf, size_t len);
    void send_bytes(const uint8_t* buf, size_t len);

    struct PgMsg { char type; std::vector<uint8_t> body; };
    PgMsg read_message();
    void  send_message(char type, const std::vector<uint8_t>& body);
    void  send_startup();
    void  do_auth();
    void  do_scram_sha256(const std::vector<uint8_t>& mechanisms_body);

    std::vector<DbRow> simple_query(const std::string& sql);
    std::vector<DbRow> extended_query(const std::string& sql, const std::vector<DbParam>& params);
    std::vector<DbRow> do_lastval();
    std::vector<DbRow> drain_rows(std::vector<ColInfo_>& columns);

    struct ColInfo_ { std::string name; uint8_t type_code; };

    static uint32_t read_u32_be(const uint8_t* p);
    static int32_t  read_i32_be(const uint8_t* p);
    static uint16_t read_u16_be(const uint8_t* p);
    static void     write_u32_be(uint8_t* p, uint32_t v);

    static std::array<uint8_t,16> md5_raw(const uint8_t* data, size_t len);
    static std::string            md5_hex(const uint8_t* data, size_t len);
    static std::string            pg_md5_password(const std::string& password,
                                                   const std::string& user,
                                                   const uint8_t salt[4]);

    // SCRAM-SHA-256 primitifleri (sıfır bağımlılık)
    static std::array<uint8_t,32> sha256(const uint8_t* data, size_t len);
    static std::array<uint8_t,32> hmac_sha256(const uint8_t* key, size_t klen,
                                               const uint8_t* msg, size_t mlen);
    static std::array<uint8_t,32> pbkdf2_sha256(const std::string& pass,
                                                  const uint8_t* salt, size_t slen, int iters);
    static std::string            base64_enc(const uint8_t* data, size_t len);
    static std::vector<uint8_t>   base64_dec(const std::string& s);

    // PostgreSQL OID → LOOK type code
    static uint8_t oid_to_type(int32_t oid);
};

} // namespace look

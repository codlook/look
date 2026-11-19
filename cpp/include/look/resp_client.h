#pragma once
#include <string>
#include <vector>
#include <stdexcept>

// Minimal RESP2 client — SET/GET/DEL/EXPIRE/EXISTS
// A sufficient subset for Session; no pipeline, pub/sub, cluster.
// TLS: LOOK_REDIS_URL=rediss://... → OpenSSL (build flag: LOOK_REDIS_TLS)

namespace look {

class RespClient {
public:
    // url: redis://[user:pass@]host:port[/db]   or   rediss://...
    explicit RespClient(const std::string& url);
    ~RespClient();

    // Connect (called automatically in the constructor; call again to reconnect)
    void connect();
    bool is_connected() const { return fd_ >= 0; }
    // An idle connection: closes it and returns false if the server closed it or there are unread bytes
    bool usable_when_idle();

    // Commands — all throw std::runtime_error on a network error
    void        set(const std::string& key, const std::string& val, int ttl_sec = 0);
    bool        set_if_exists(const std::string& key, const std::string& val, int ttl_sec = 0);  // SET XX; false = no such key
    std::string get(const std::string& key);      // returns "" if not found
    bool        get_if_exists(const std::string& key, std::string& out);  // false = no such key (nil); an empty value is true
    void        del(const std::string& key);
    bool        exists(const std::string& key);
    void        expire(const std::string& key, int ttl_sec);

private:
    std::string host_;
    int         port_  = 6379;
    int         db_    = 0;
    std::string pass_;
    bool        tls_        = false;  // rediss:// or ?tls=1 → TLS (SSL_connect at the start of the connection)
    bool        tls_verify_ = false;  // ?tls=verify → SSL_VERIFY_PEER + hostname (against MITM)
    int         fd_    = -1;
    void*       ssl_   = nullptr;     // SSL* (POSIX/OpenSSL); when TLS is active all I/O goes through here
    int         tmo_ms_ = 3000;       // connect/send/reply time limit (LOOK_REDIS_TIMEOUT_MS)
    std::string rbuf_;                // read buffer (so that lines are not read with a system call per byte)
    size_t      rpos_ = 0, rlen_ = 0;

    void        drop();                                   // close the connection (never used again)
    [[noreturn]] void fail(const std::string& msg);      // drop + throw
    [[noreturn]] void fail_timeout(const char* what);    // drop + timeout error
    void        fill();                                   // fill the buffer (time-limited)
    void        send_command(const std::vector<std::string>& args);
    std::string read_line();
    std::string read_bulk(long len);          // long: so that a >INT_MAX bulk does not overflow int
    std::string read_response(int depth = 0);  // depth: deep array → stack overflow guard
};

} // namespace look

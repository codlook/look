#pragma once
#include <string>
#include <vector>
#include <stdexcept>

// Minimal RESP2 client — SET/GET/DEL/EXPIRE/EXISTS
// Session için yeterli subset; pipeline, pub/sub, cluster yok.
// TLS: LOOK_REDIS_URL=rediss://... → OpenSSL (derleme flag: LOOK_REDIS_TLS)

namespace look {

class RespClient {
public:
    // url: redis://[user:pass@]host:port[/db]   veya   rediss://...
    explicit RespClient(const std::string& url);
    ~RespClient();

    // Bağlan (constructor'da otomatik çağrılır; reconnect için tekrar çağır)
    void connect();
    bool is_connected() const { return fd_ >= 0; }

    // Komutlar — hepsi network hatasında std::runtime_error fırlatır
    void        set(const std::string& key, const std::string& val, int ttl_sec = 0);
    std::string get(const std::string& key);      // bulunamazsa "" döner
    void        del(const std::string& key);
    bool        exists(const std::string& key);
    void        expire(const std::string& key, int ttl_sec);

private:
    std::string host_;
    int         port_  = 6379;
    int         db_    = 0;
    std::string pass_;
    bool        tls_   = false;
    int         fd_    = -1;

    void        send_command(const std::vector<std::string>& args);
    std::string read_line();
    std::string read_bulk(long len);          // long: >INT_MAX bulk int'e taşmasın
    std::string read_response(int depth = 0);  // depth: derin dizi → stack overflow guard
};

} // namespace look

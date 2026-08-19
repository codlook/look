#pragma once
#include <string>
#include <map>
#include <functional>

namespace look {

// Streaming: govde parcalari (chunked cozulmus) geldikce cagrilir.
using HttpChunkCallback = std::function<void(const std::string&)>;

// ODR KRITIK: bu tip ISTEMCI tarafina aittir ve adi http_server.hdeki
// look::HttpResponse ile ASLA ayni olmamali. Eskiden ikisi de "look::HttpResponse"
// idi → look-fcgi ikisini de linkledigi icin (look/CLI yalnizca istemciyi linkler)
// TANIMSIZ DAVRANIS; LTO acikken iki farkli layout tek tip sanilip birlestiriliyor,
// nesne bir duzenle kurulup digeriyle yikiliyordu → "free(): invalid pointer" /
// segfault. Web routeundan http::get cagirmak sunucuyu COKERTIYORDU (CLIde degil,
// ASanda degil — ASan buildinde LTO kapali oldugu icin gizleniyordu).
struct HttpClientResponse {
    int         status  = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;   // empty = no error
};

struct HttpOptions {
    int  timeout_ms       = 10000;   // default 10s
    bool follow_redirects = false;   // opt-in: follow 3xx Location headers
    int  max_redirects    = 5;       // hop cap when following redirects
};

// Parses "https://host:port/path?query" into parts
struct ParsedUrl {
    bool        tls  = false;
    std::string host;
    int         port = 80;
    std::string path;   // includes query string
};

ParsedUrl   parse_url(const std::string& url);
HttpClientResponse http_request(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& req_headers,
    const HttpOptions& opts
);

// Streaming istek: govde parcalari geldikce on_chunk cagrilir (SSE/token akisi).
// Doner: status + headers (body genelde bostur — parcalar callback'e gitti).
HttpClientResponse http_request_stream(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& req_headers,
    const HttpOptions& opts,
    const HttpChunkCallback& on_chunk
);

// Sistem CA bundle'ını (SSL_CERT_FILE/DIR) tespit et — statik OpenSSL binary'nin
// https doğrulaması için. main() başında bir kez çağrılır. Zaten set ise dokunmaz.
void configure_system_ca_bundle();

} // namespace look

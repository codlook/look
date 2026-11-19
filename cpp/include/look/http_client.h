#pragma once
#include <string>
#include <map>
#include <functional>

namespace look {

// Streaming: called as body chunks (chunked decoded) arrive.
using HttpChunkCallback = std::function<void(const std::string&)>;

// ODR CRITICAL: this type belongs to the CLIENT side and its name must NEVER be the same as
// look::HttpResponse in http_server.h. Both used to be "look::HttpResponse"
// → since look-fcgi links both (look/CLI link only the client) this was
// UNDEFINED BEHAVIOUR; with LTO on, two different layouts were taken for one type and merged,
// an object was built with one layout and destroyed with the other → "free(): invalid pointer" /
// segfault. Calling http::get from a web route CRASHED the server (not in the CLI,
// not under ASan — it was hidden because LTO is off in the ASan build).
struct HttpClientResponse {
    int         status  = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;   // empty = no error
};

struct HttpOptions {
    int         timeout_ms       = 10000;   // default 10s
    bool        follow_redirects = false;   // opt-in: follow 3xx Location headers
    int         max_redirects    = 5;       // hop cap when following redirects
    std::string proxy;                       // "http://host:port" — routes traffic through a proxy
};

// Parses "https://host:port/path?query" into parts
struct ParsedUrl {
    bool        tls  = false;
    std::string host;
    int         port = 80;
    std::string path;   // includes query string
    std::string basic_auth;   // "user:pass@" in the URL → base64 for "Authorization: Basic" (Go URL.User)
};

ParsedUrl   parse_url(const std::string& url);
HttpClientResponse http_request(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& req_headers,
    const HttpOptions& opts
);

// Streaming request: on_chunk is called as body chunks arrive (SSE/token stream).
// Returns: status + headers (body is usually empty — the chunks went to the callback).
HttpClientResponse http_request_stream(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& req_headers,
    const HttpOptions& opts,
    const HttpChunkCallback& on_chunk
);

// Detect the system CA bundle (SSL_CERT_FILE/DIR) — for the static OpenSSL binary's
// https verification. Called once at the start of main(). Does not touch it if already set.
void configure_system_ca_bundle();

} // namespace look

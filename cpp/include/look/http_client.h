#pragma once
#include <string>
#include <map>

namespace look {

struct HttpResponse {
    int         status  = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;   // empty = no error
};

struct HttpOptions {
    int timeout_ms = 10000;   // default 10s
};

// Parses "https://host:port/path?query" into parts
struct ParsedUrl {
    bool        tls  = false;
    std::string host;
    int         port = 80;
    std::string path;   // includes query string
};

ParsedUrl   parse_url(const std::string& url);
HttpResponse http_request(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& req_headers,
    const HttpOptions& opts
);

} // namespace look

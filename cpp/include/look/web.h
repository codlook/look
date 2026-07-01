#pragma once

#include <string>
#include <map>
#include <set>
#include <vector>

namespace look {

// ── SVG Sanitizer ─────────────────────────────────────────────────────────────
// Single source of truth — used by both production (file::store allow_svg:true)
// and the test runner (html::sanitize_svg).
//
// To add a safe element: add its lower-case name to SVG_SAFE_ELEMENTS and
// recompile. Both production and test binaries pick up the change.

inline const std::set<std::string> SVG_SAFE_ELEMENTS = {
    "svg", "g", "path", "circle", "rect", "ellipse",
    "line", "polyline", "polygon",
    "text", "tspan",
    "defs", "symbol", "title", "desc",
    "lineargradient", "radialgradient", "stop",
    "clippath", "mask", "pattern",
    "use"
    // Excluded: script, foreignobject, animate, set, image, feimage,
    //           iframe, embed, object, style
};

// Sanitize raw SVG markup. Never throws on malformed input.
std::string svg_sanitize(const std::string& input);

// ── Uploaded File ─────────────────────────────────────────────────────────────

struct UploadedFile {
    std::string field_name;     // form field adı
    std::string temp_path;      // temp dosya yolu
    std::string mime;           // doğrulanmış MIME (magic byte'dan)
    std::string sha256;         // dosya içeriğinin SHA-256 hex hash'i
    size_t      size = 0;       // byte
    bool        valid = false;  // parse başarılı mı
};

// ── Web Context ───────────────────────────────────────────────────────────────
// CGI isteğinin tüm bilgilerini tutar. Interpreter tarafından paylaşılır.

struct WebContext {
    // Request
    std::string method;
    std::string path;
    std::string query_string;
    std::string body;
    std::string remote_addr;
    std::string content_type;
    std::map<std::string, std::string> get_params;
    std::map<std::string, std::string> post_params;
    std::map<std::string, std::string> cookies_in;
    std::map<std::string, UploadedFile> uploaded_files;  // multipart dosyaları

    // Response
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers_out;
    bool headers_sent = false;

    // Routing
    bool route_matched = false;
    bool not_found_handler_registered = false;
    std::map<std::string, std::string> route_params;

    // Initialize from CGI environment
    void init_from_cgi();

    // Multipart parser — init_from_cgi tarafından çağrılır
    void parse_multipart(const std::string& boundary);

    // Helpers
    static std::string url_decode(const std::string& s);
    static std::map<std::string, std::string> parse_query(const std::string& qs);
    static std::map<std::string, std::string> parse_cookies(const std::string& raw);

    // Header output
    std::string build_headers() const;
    void set_status(int code);
};

// Route matched — used as flow control exception
class RouteMatchedException : public std::exception {
    const char* what() const noexcept override { return "route_matched"; }
};

// ── Connection pool — thread-safe, per-request borrow/return ──────────────────
// Called by fcgi_main before/after each request dispatch in threaded mode.
void acquire_thread_connections();   // borrow one conn per pool into thread-local
void release_thread_connections();   // return all thread-local conns to pools
void set_db_pool_size(int n);        // set pool size before any db::connect call
void clear_db_pools();               // close all pools — called before hot reload

} // namespace look

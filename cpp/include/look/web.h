#pragma once

#include <string>
#include <map>
#include <set>
#include <vector>
#include <cstdio>
#include <memory>

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

// Temp-file RAII guard — deletes the file when the last reference (shared_ptr) goes away.
// UploadedFile stays copyable (WebContext is copied); thanks to the shared refcount
// the file is deleted only when ALL copies are gone (no early deletion).
struct TempFileGuard {
    std::string path;
    ~TempFileGuard() { if (!path.empty()) std::remove(path.c_str()); }
};

struct UploadedFile {
    std::string field_name;     // form field name
    std::string orig_name;      // filename sent by the client (basename, sanitized — UNTRUSTED, escape on use)
    std::string temp_path;      // temp file path
    std::string mime;           // validated MIME (from magic bytes)
    std::string sha256;         // SHA-256 hex hash of the file contents
    size_t      size = 0;       // bytes
    bool        valid = false;  // whether parsing succeeded
    // When the WebContext is destroyed, a temp file the app did NOT consume with file::store() is deleted
    // (otherwise disk/inode exhaustion). If file::store moved the file, std::remove
    // is a no-op → harmless. The parser sets this for every new temp file.
    std::shared_ptr<TempFileGuard> temp_cleanup;
};

// ── Web Context ───────────────────────────────────────────────────────────────
// Holds all information about a CGI request. Shared by RuntimeHost and the VM.

struct WebContext {
    // Request
    std::string method;
    std::string path;
    std::string query_string;
    std::string body;
    std::string remote_addr;
    std::string content_type;
    // True only when the client connection is actually HTTPS — set from an HTTPS FCGI param, or
    // X-Forwarded-Proto=https from a TRUSTED proxy (LOOK_TRUSTED_PROXY). The session cookie's
    // Secure flag is gated on this: Secure only helps under TLS, so setting it over plain HTTP
    // just stops the cookie from being sent (which silently breaks local http:// dev).
    bool is_https = false;
    // Request host (request::host, csrf:: Origin check). Same rule as is_https:
    // in --mode http the Host header; X-Forwarded-Host only if the connecting address is in LOOK_TRUSTED_PROXY.
    // In FastCGI/CGI the web server's HTTP_HOST (the server in front is the trusted party).
    // Invalid value (space, control character, '/', '@' …) → "" (clean_host).
    std::string host;
    // csrf:: — key base used for this request (the cookie does not come back in cookies_in within the same request).
    std::string document_root;   // FastCGI/CGI DOCUMENT_ROOT (file::store web-root check); empty in http mode
    std::string csrf_base;
    // The csrf::exempt rule if this request passed unchecked through it (session:: is forbidden here).
    std::string csrf_exempt_rule;
    // Session read in session::start (id + content): if a concurrent regenerate deleted the old record
    // before this request, the data is carried from here (scs model). Filled only for an id that existed at request start.
    std::string sess_snap_sid;
    std::string sess_snap;
    std::map<std::string, std::string> get_params;
    std::map<std::string, std::string> post_params;
    std::map<std::string, std::string> cookies_in;
    std::map<std::string, std::string> headers_in;   // request headers, lowercase keys
    std::map<std::string, UploadedFile> uploaded_files;  // multipart files

    // Response
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers_out;
    std::vector<std::string> set_cookies_out;  // Set-Cookie: a std::map would overwrite the single key;
                                               // each cookie becomes a separate line.
    std::string response_body;   // set by response::json/text/html
    bool headers_sent = false;

    // Routing
    bool route_matched = false;
    bool not_found_handler_registered = false;
    std::map<std::string, std::string> route_params;

    // Initialize from CGI environment
    void init_from_cgi();

    // Multipart parser — called by init_from_cgi
    void parse_multipart(const std::string& boundary);

    // Parse the POST body by content_type: urlencoded → post_params,
    // multipart/form-data → parse_multipart (files + fields).
    // SINGLE SOURCE: all three entry points (FastCGI, --mode http, CGI) call this.
    // Previously the same 6 lines were COPIED in fcgi_main and web.cpp, while http_main had
    // only the urlencoded branch → `request::file` worked under FastCGI but
    // not under `--mode http` (a mode-specific gap, S12).
    void parse_post_body();
    // If the body could not be parsed: response status (400 / 413) and reason. Dispatch sends this response before routing and the CSRF
    // gate (explicit error like Go mime/multipart; previously silently empty).
    int         body_error_status = 0;
    std::string body_error;

    // Helpers
    // plus_is_space: query strings encode space as '+', but in a PATH segment '+' is literal.
    static std::string url_decode(const std::string& s, bool plus_is_space = true);
    static std::map<std::string, std::string> parse_query(const std::string& qs);
    static std::map<std::string, std::string> parse_cookies(const std::string& raw);
    // Validate the Host header value + lowercase it: name[:port] or [ipv6][:port]; otherwise "".
    static std::string clean_host(const std::string& raw);

    // Header output
    std::string build_headers() const;
    void set_status(int code);
};

// Route matched — used as flow control exception
class RouteMatchedException : public std::exception {
    const char* what() const noexcept override { return "route_matched"; }
};

// stop() — cancels route execution from before_route middleware
class RouteStopException : public std::exception {
    const char* what() const noexcept override { return "route_stop"; }
};

// ── Connection pool — thread-safe, per-request borrow/return ──────────────────
// The connection is borrowed lazily on the first db:: call; all are returned at the end of the request/callback.
void release_thread_connections();   // return all conns borrowed by this request to pools
void set_db_pool_size(int n);        // set pool size before any db::connect call
void clear_db_pools();               // close all pools — called before hot reload
// Any path that touches a cookie in a request exempted via csrf::exempt raises an error (what: "cookie::get()" …).
void csrf_exempt_cookie_guard(const WebContext* w, const char* what);

// Available CPU count — honors the cgroup CPU limit (container/Docker/systemd CPUQuota,
// constrained cloud vCPU). std::thread::hardware_concurrency() returns PHYSICAL
// cores; in a constrained environment the worker/pool default would spawn too many threads and waste
// RAM. Worker and DB-pool defaults use this → automatic fit to the environment.
int available_cpus();
// CFS CPU quota (cgroup) rounded to whole cores, or -1 if no quota is set.
// For the startup warning: workers > quota → CFS throttling spikes tail latency.
int cfs_quota_cpus();

} // namespace look

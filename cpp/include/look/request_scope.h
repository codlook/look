#pragma once
// Request handle — the context and output of the request running right now.
//
// The web modules (request::, response::, cookie::, session::, csrf::, route::) and print do not capture the
// context; they read it from here at the moment they are called. So the modules and the builtin table are set up once per
// thread. They used to be set up again on every request (because the context's address on the fiber
// stack changed per request) — measured: most of the request cost, the VM itself 2%.
//
// Where it is stored: inside a fiber Fiber::current()->local (each fiber's own request — a suspended
// request does not see a request that later starts on the same thread); outside a fiber (worker pool, FastCGI,
// CGI, timer thread) thread-local. RequestScope saves the previous value and restores it —
// nested dispatch (timer/WS callback) does not disturb the outer request.
#include <ostream>

namespace look {

struct WebContext;

// The current request's context / output; nullptr if there is no request.
WebContext*   current_request_web();
std::ostream* current_request_out();

class RequestScope {
public:
    RequestScope(WebContext* web, std::ostream* out);
    ~RequestScope();
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;
private:
    WebContext*   prev_web_;
    std::ostream* prev_out_;
};

} // namespace look

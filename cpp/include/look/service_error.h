#pragma once
// A dependent service (such as Redis, the session store) does not answer or is unreachable: the request
// is not the application's fault but a temporary service outage. If LOOK code does not catch it the dispatcher
// returns 503 Service Unavailable instead of 500 and writes the error to the log. LOOK try/catch sees it
// as an ordinary error message.
#include <stdexcept>
#include <string>

namespace look {

class ServiceUnavailable : public std::runtime_error {
public:
    explicit ServiceUnavailable(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace look

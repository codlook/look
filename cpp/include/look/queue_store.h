#pragma once
#include "interpreter.h"
#include <string>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <optional>

namespace look {

// Named, thread-safe, non-blocking FIFO queues.
// Global singleton — persists across FastCGI requests (warm start).
// Unlike channel (goroutine blocking), queue:: is for cross-request job queuing.
class QueueStore {
public:
    static QueueStore& instance();

    void   push(const std::string& name, Value val);
    Value  pop(const std::string& name);      // null if empty
    Value  peek(const std::string& name);     // null if empty, no remove
    int    size(const std::string& name);
    void   clear(const std::string& name);
    std::vector<std::string> names();

private:
    std::unordered_map<std::string, std::deque<Value>> queues_;
    mutable std::mutex mtx_;
};

Module make_queue_module();

} // namespace look

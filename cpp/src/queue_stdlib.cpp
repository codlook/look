// queue_stdlib.cpp — queue:: module (named, thread-safe, non-blocking FIFO)
#include "look/queue_store.h"
#include <algorithm>

namespace look {

// ── Singleton ─────────────────────────────────────────────────────────────
QueueStore& QueueStore::instance() {
    static QueueStore inst;
    return inst;
}

void QueueStore::push(const std::string& name, Value val) {
    std::lock_guard<std::mutex> lk(mtx_);
    queues_[name].push_back(std::move(val));
}

Value QueueStore::pop(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = queues_.find(name);
    if (it == queues_.end() || it->second.empty()) return Value();
    Value v = std::move(it->second.front());
    it->second.pop_front();
    return v;
}

Value QueueStore::peek(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = queues_.find(name);
    if (it == queues_.end() || it->second.empty()) return Value();
    return it->second.front();
}

int QueueStore::size(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = queues_.find(name);
    if (it == queues_.end()) return 0;
    return (int)it->second.size();
}

void QueueStore::clear(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    queues_.erase(name);
}

std::vector<std::string> QueueStore::names() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> result;
    result.reserve(queues_.size());
    for (auto& [k, q] : queues_)
        if (!q.empty()) result.push_back(k);
    std::sort(result.begin(), result.end());
    return result;
}

// ── LOOK module ────────────────────────────────────────────────────────────
Module make_queue_module() {
    Module m;
    m.name = "queue";

    // queue::push($name, $val) — add to end
    m.functions["push"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw std::runtime_error("queue::push() — (name, value) bekler");
        QueueStore::instance().push(args[0].to_string(), args[1]);
        return Value(true);
    };

    // queue::pop($name) → value | null
    m.functions["pop"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("queue::pop() — name bekler");
        return QueueStore::instance().pop(args[0].to_string());
    };

    // queue::peek($name) → value | null (no remove)
    m.functions["peek"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("queue::peek() — name bekler");
        return QueueStore::instance().peek(args[0].to_string());
    };

    // queue::size($name) → int
    m.functions["size"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("queue::size() — name bekler");
        return Value(QueueStore::instance().size(args[0].to_string()));
    };

    // queue::clear($name) → removes all items from named queue
    m.functions["clear"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("queue::clear() — name bekler");
        QueueStore::instance().clear(args[0].to_string());
        return Value(true);
    };

    // queue::names() → string[] (non-empty queues, sorted)
    m.functions["names"] = [](std::vector<Value> args) -> Value {
        auto ns = QueueStore::instance().names();
        auto arr = std::make_shared<std::vector<Value>>();
        for (auto& n : ns) arr->push_back(Value(n));
        return Value(arr);
    };

    return m;
}

} // namespace look

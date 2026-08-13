// route_group.cpp — route::group() context'inin tek tanımı. Bkz. route_group.h.
// Setup tek-thread'li (run_setup_http) → thread_local güvenli ve yeterli.
#include "look/route_group.h"

namespace look {

namespace {
struct Frame { std::string prefix; std::vector<Value> mws; };
thread_local std::vector<Frame> g_stack;
}

void route_group_push(const std::string& prefix, std::vector<Value> mws) {
    g_stack.push_back(Frame{prefix, std::move(mws)});
}

void route_group_pop() {
    if (!g_stack.empty()) g_stack.pop_back();
}

bool route_group_active() { return !g_stack.empty(); }

std::string route_group_prefix() {
    std::string p;
    for (auto& f : g_stack) p += f.prefix;   // en dış → en iç
    return p;
}

std::vector<Value> route_group_middlewares() {
    std::vector<Value> out;
    for (auto& f : g_stack)                   // en-dış-önce
        for (auto& mw : f.mws) out.push_back(mw);
    return out;
}

} // namespace look

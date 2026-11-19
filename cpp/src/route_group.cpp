// route_group.cpp — the single definition of the route::group() context. See route_group.h.
// Setup is single-threaded (run_setup_http) → thread_local is safe and sufficient.
#include "look/route_group.h"

namespace look {

namespace {
struct Frame { std::string prefix; std::vector<Value> mws; };
// fiber-ok: load time only (the route::group body runs during setup, no suspension)
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
    for (auto& f : g_stack) p += f.prefix;   // outermost → innermost
    return p;
}

std::vector<Value> route_group_middlewares() {
    std::vector<Value> out;
    for (auto& f : g_stack)                   // outermost first
        for (auto& mw : f.mws) out.push_back(mw);
    return out;
}

} // namespace look

// route_group.h -- setup-time context of route::group() (Go/chi style prefix + middleware inheritance).
// route::group("/admin", [$auth], fn) -> route() calls inside fn() INHERIT the prefix and the middlewares.
// ONLY a registration-time transformation: it does not touch dispatch (the prefix matches normally, the mws
// already run). Nesting is natural via a stack (Go/chi). No name/namespace/domain -- deliberately minimal.
// Single definition (route_group.cpp) -> shared by builtins.cpp and http_main.cpp (no drift).
#pragma once
#include <string>
#include <vector>
#include "look/runtime_host.h"   // look::Value

namespace look {

// Push a group frame onto the stack (on entering route::group). mws: this group's middlewares.
void route_group_push(const std::string& prefix, std::vector<Value> mws);
// Pop the frame (on leaving route::group -- when the closure call finishes).
void route_group_pop();
// Are we currently inside a group?
bool route_group_active();
// Concatenation of all active group prefixes (outermost -> innermost), e.g. "/admin" + "/users" = "/admin/users".
std::string route_group_prefix();
// All active group middlewares, flattened outermost-first.
std::vector<Value> route_group_middlewares();

} // namespace look

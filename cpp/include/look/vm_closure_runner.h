#pragma once
// Runs a VM closure like a NEW REQUEST (its own VM, the declared values of the globals):
// timer callbacks and jobs::run handlers use it. The web server and the command line
// (cli_runtime) install it. If ok=false the error has been logged. Empty if not installed.
#include "look/value.h"
#include <functional>
#include <vector>

namespace look {

using VmClosureRunner = std::function<Value(const Value& fn, std::vector<Value> args, const char* what, bool& ok)>;
inline VmClosureRunner& vm_closure_runner() { static VmClosureRunner r; return r; }

} // namespace look

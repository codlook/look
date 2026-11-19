#pragma once
// Command-line VM environment — `lk` and `lk test` use the SAME built-in table and callback bindings
// (src/cli_runtime.cpp). A single source so that tests exercise the environment the program really runs in.
#include "look/runtime_host.h"
#include "look/vm.h"
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace look {

// Built-in table: functions of the `use`d modules (from interp) + bare built-ins; print/write → out.
std::vector<BuiltinFn> build_cli_builtins(RuntimeHost& interp, std::ostream& out);

// Shared state for job handler / timer / parallel callbacks.
struct CliCallbacks {
    VM::SharedState* shared  = nullptr;   // built-in table (process lifetime)
    VM*              main_vm = nullptr;   // globals current at jobs::run time
};

// Runs a VM closure on its own VM (an error is logged, ok=false). If globals is empty, the main VM's are used.
Value run_cli_closure(const CliCallbacks& cc, const Value& fnv, std::vector<Value> args,
                      const std::unordered_map<std::string, Value>* globals,
                      const char* method, const char* what, bool& ok);

// timer::/jobs::worker/jobs::run on the command line go to the VM; ws::/sse:: give an explicit error.
void wire_cli_callbacks(std::vector<BuiltinFn>& b, RuntimeHost& interp, std::shared_ptr<CliCallbacks> cc);

} // namespace look

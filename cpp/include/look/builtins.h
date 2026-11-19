#pragma once

// builtins.h -- the SINGLE source of the VM builtin table
//
// This table keeps two consumers in sync:
//   1. compiler.cpp  -- name -> builtin index (emits the CALL_BUILTIN opcode)
//   2. http_main.cpp -- index -> runtime function (req_builtins wiring)
//
// Rule: names of the form "mod::fn" are wired AUTOMATICALLY on the http_main side
// (through the RuntimeHost module function). Names without "::" (print,
// count, strlen, route...) carry special semantics and are bound by hand in http_main.
//
// ADDING A NEW BUILTIN: append it to the END of the list (existing indexes are embedded in
// .lkc bytecode caches -- inserting in the middle breaks all caches). For "mod::fn" nothing
// else is needed; for a special name, add a lambda in http_main.

#include <string>
#include <vector>

namespace look {

// Canonical builtin name list -- index == vector position
const std::vector<std::string>& builtin_names();

// CLI arguments -- the words AFTER the script in `lk script.lk <args...>`.
// main() fills it once; the args() builtin reads from here (read-only, process-global).
std::vector<std::string>& script_args();

// Name -> index; -1 if not found. O(1) (a hash map is built on the first call).
int builtin_index(const std::string& name);

// Is it a reserved bare builtin name? (a user function CANNOT SHADOW this name)
//
// The source is PROGRAMMATIC: builtin_names() entries WITHOUT "::" (print, count,
// route, config, env, stop, before_route, strlen, abs...). No hand list is kept --
// it derives from the dispatcher's single source; a new bare builtin is covered automatically.
//
// WHY: the compiler dispatches a bare builtin BEFORE a user function ->
// `function config() {...}` silently became dead code. Now it is an error at definition time.
bool is_reserved_builtin(const std::string& name);

// A builtin's effect -- EVERY builtin_names() entry MUST be classified (CI:
// tests/builtin_effect_table_test.sh; a new unclassified builtin -> red). Consumer: the rule
// for a pure builtin whose result is discarded (look/unused_result.h) -- applies to Pure only.
//   Pure       does not modify its argument, no outside effect, only returns a value
//              (array::push returns a new array -> Pure; global push modifies the array IN PLACE -> Effect)
//   Reads      reads outside state (request, session, cache, clock, random, environment)
//   Effect     changes or writes something (output, database, file, network, its argument)
//   Callback   takes a callback -- the callback may have effects (array::map ...)
//   ThrowCheck can be called on its own to check "does it throw" (json::decode, date::parse ...)
enum class BuiltinEffect { Unknown, Pure, Reads, Effect, Callback, ThrowCheck };
BuiltinEffect builtin_effect(const std::string& name);
// Entries named in the classification but absent from builtin_names() (typo check -- CI).
std::vector<std::string> builtin_effect_strays();

} // namespace look

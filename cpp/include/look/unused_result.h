#pragma once
// A pure builtin call whose result is unused: `array::push($list, $x)` as a standalone statement.
// array::push does NOT modify the list in place, it returns a new list -- when the result is discarded nothing happens
// and no error appears (in the VS Code example the note was never saved; even the author of the example fell for it).
// Which builtin is pure is NOT kept in a hand-maintained list but next to the builtin table
// (src/builtins.cpp: builtin_effect) -- every builtin must be classified, a new unclassified
// builtin is red in CI (tests/builtin_effect_table_test.sh). The rule applies to Pure only:
// builtins that take callbacks (Callback) and ones that can be called on their own to ask "does it throw" (ThrowCheck)
// are OUT; ones that read outside state (Reads) are also out for now.
#include "look/builtins.h"
#include <string>

namespace look {

inline bool is_pure_builtin(const std::string& full) {
    return builtin_effect(full) == BuiltinEffect::Pure;
}

// User-facing explanation (English). The "does not modify the list" emphasis for array::*,
// since that is the most common trap.
inline std::string unused_result_message(const std::string& full, int line) {
    std::string at = line > 0 ? "line " + std::to_string(line) + ": " : "";
    // array::pop returns the last element, array::contains a bool -- saying "new array" would be wrong.
    const bool arr = full.rfind("array::", 0) == 0;
    const bool returns_array = arr && full != "array::pop" && full != "array::contains";
    std::string why = returns_array ? full + "() returns a new array and does not change the one you pass"
                    : arr           ? full + "() returns a value and does not change the array you pass"
                    :                 full + "() returns a value and has no other effect";
    return at + "the result of " + full + "() is not used — " + why +
           ". Assign the result, e.g. $x = " + full + "(...)";
}

} // namespace look

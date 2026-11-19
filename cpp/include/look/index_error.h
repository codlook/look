#pragma once
// Indexing rule and error text: SINGLE definition.
//
// Rule (like map reads in Go, in one sentence):
//   * a KEY missing in an array/map  -> null
//   * indexing null                  -> null   (null reads like an empty map; reading a nil map in Go
//                                              is not an error). `$row["address"]["city"] ?? ""`
//                                              therefore works on its own -- `??` has no
//                                              special meaning here.
//   * indexing any other value that is NOT an array, map or struct (string, number, bool, function)
//                                    -> ERROR   (this is not a missing value, it is the wrong type)
// In 1.x the interpreter raised an error on all of them including null, while the VM silently returned null on all of them:
// the same line behaved differently per engine, and a wrong type propagated as null.
#include "look/runtime_host.h"
#include <string>

namespace look {

inline std::string index_error_message(const Value& target, const Value& key) {
    const char* kind =
        target.type() == Value::STRING ? "a string" :
        target.type() == Value::INT    ? "an integer" :
        target.type() == Value::FLOAT  ? "a float" :
        target.type() == Value::BOOL   ? "a boolean" : "a value that is not an array";
    const std::string k = key.type() == Value::STRING ? "\"" + key.to_string() + "\"" : key.to_string();
    return "Index operator requires an array: cannot read [" + k + "] of " + kind
         + " — only arrays, maps and structs can be indexed (a missing key and null both read as null)";
}

} // namespace look

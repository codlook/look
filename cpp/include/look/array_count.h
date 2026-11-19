#pragma once
#include "look/runtime_host.h"   // Value

namespace look {

// count() / len() semantics -- SINGLE definition (CLI VM + web VM must give the same result).
//
// OLD BUG (VM<->interpreter divergence, invisible to differential testing because the tests used PLAIN arrays):
// the old interpreter counted the assoc sentinel but the three VM builtin tables (main.cpp b[2],
// http_main setup_builtins[2] + req_builtins[2]) SKIPPED it -> count(["x"=>1]) returned 3 (sentinel+2)
// instead of 1. Result: `if (count($assoc)==0)` was never true, count(request::json())
// was silently wrong. Assoc = ["__assoc__", k0,v0,...] -> pair count = (size-1)/2.
//
// This helper reduces the four call sites to a single definition (same pattern as path_guard.h / format.h / html_escape.h).
// ROOT: the sentinel design itself (a separate Value::Type::ASSOC type would close this completely).
inline int look_count(const Value& v) {
    if (v.type() == Value::ARRAY) {
        auto& arr = *v.as_array();
        if (!arr.empty() && arr[0].is_assoc_marker())
            return (int)((arr.size() - 1) / 2);
        return (int)arr.size();
    }
    // String: count of CHARACTERS (code points) -- same as string::len. `len` maps to count in the compiler
    // and is documented as an alias of string::len; it used to count bytes: len("éé") = 4
    // (found by a randomized comparison test). Continuation bytes (10xxxxxx) are not counted.
    if (v.type() == Value::STRING) {
        int n = 0;
        for (unsigned char c : v.as_string()) if ((c & 0xC0) != 0x80) ++n;
        return n;
    }
    // A struct is not an array: it used to silently return 0.
    if (v.type() == Value::STRUCT)
        throw std::runtime_error("count() does not take a struct ('" + (*v.as_struct())[0].str_ref()
                                 + "'): a struct is not an array, use its fields");
    return 0;
}

} // namespace look

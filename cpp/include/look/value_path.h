#pragma once
// Value semantics: copy-on-write assignment along an index PATH.
//
//   $root[k0][k1]...[kn-1] = value
//
// Rule: arrays, maps and structs are VALUES; assigning/passing/returning gives the target its own value.
// Implementation: storage is shared and reference-counted; a copy is made only when WRITING to a
// storage that is still shared. For this to work the write must start at the variable's OWN slot
// and descend, making every level uniquely owned there. Copying the array into a temporary first
// and writing through it (what 1.x did) makes the temporary a second owner: either every write
// copies (quadratic loop) or the write is lost.
//
// Element lookup (value_slot) and the path walk (value_set_path) live in one place. The write at
// the last level is the VM's single-level assignment (VM::array_set), passed in as a callback.
#include "look/struct_types.h"
#include "look/assoc_index.h"

namespace look {

// Is this a WHOLE-NUMBER string such as "12" / "-3"? Distinguishes a numeric array index from a
// map key. (It used to be two separate copies in vm.cpp and interpreter.cpp.)
inline bool value_is_int_key(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// The SLOT of an existing element; nullptr if absent (does not throw -- path assignment creates a missing middle level).
// Lookup rules match reads: map key; integer string = numeric index; negative = from the
// end. Struct: field name; an unknown field or a numeric index is an ERROR (fixed shape).
inline Value* value_slot(Value& arr, const Value& key) {
    if (arr.type() == Value::STRUCT) {
        auto& sv = *arr.vec_ptr();
        if (key.type() != Value::STRING)
            throw std::runtime_error("a struct is not an array: use a field name ('"
                                     + struct_name(sv) + "' has no numeric index)");
        int i = struct_field_index(struct_def(sv), key.str_ref());
        if (i < 0) struct_no_field(struct_name(sv), key.str_ref());
        return &sv[STRUCT_SLOT0 + (size_t)i];
    }
    if (arr.type() != Value::ARRAY) return nullptr;
    auto& vec = *arr.vec_ptr();
    if (!vec.empty() && vec[0].is_assoc_marker()) {
        std::string ktmp;
        const std::string& k = (key.type() == Value::STRING) ? key.str_ref() : (ktmp = key.to_string());
        // Callers (path assignment) detach the storage first -> uniquely owned: write-path lookup.
        size_t pos = assoc_find_for_write(vec, k);
        return pos ? &vec[pos + 1] : nullptr;
    }
    if (key.type() == Value::STRING && !value_is_int_key(key.str_ref())) return nullptr;
    int64_t i = key.to_int();
    if (i < 0) i += (int64_t)vec.size();
    if (i < 0 || i >= (int64_t)vec.size()) return nullptr;
    return &vec[(size_t)i];
}

// Writes the last level of slot[k0][k1]...[kn-1] with `set_one(container, key, nullptr)`.
//   * every level is detached first (the parent level is copied if shared);
//   * a missing or null middle level is created (a map if the next key is a map key, otherwise a list);
//   * a struct field is typed, so nothing is written INSIDE a null field -- error.
template <class SetOne>
inline void value_set_path(Value& slot, const Value* keys, int n, SetOne&& set_one) {
    slot.detach();
    if (n == 1) { set_one(slot, keys[0], nullptr); return; }
    Value* child = value_slot(slot, keys[0]);
    if (slot.type() == Value::STRUCT) {
        if (child->type() == Value::NONE)
            throw std::runtime_error("struct '" + struct_name(*slot.vec_ptr()) + "': field '"
                                     + keys[0].to_string() + "' is null; assign a value to it first");
    } else if (!child || child->type() == Value::NONE) {
        // Every level from here on will be newly created: the first write to a new list may only be at index 0.
        // If the rest of the path breaks that, raise the error WITHOUT creating anything. OLD: the middle level was
        // created first and the write failed afterwards -- even when the error was caught, the variable stayed modified:
        // $m["q"][3] = 1 (inside try) -> $m = {"a":1,"q":[]} (found by a randomized comparison test).
        for (int j = 1; j < n; ++j) {
            const Value& k = keys[j];
            const bool int_key = k.type() == Value::INT ||
                                 (k.type() == Value::STRING && value_is_int_key(k.str_ref()));
            if (int_key && k.to_int() != 0)
                throw std::runtime_error("Array index " + std::to_string(k.to_int()) + " out of bounds");
        }
        const bool map_key = keys[1].type() == Value::STRING && !value_is_int_key(keys[1].str_ref());
        auto fresh = std::make_shared<ValueArray>();
        if (map_key) fresh->push_back(Value::assoc_marker());
        Value holder(fresh);
        // Place the middle level with the engine's own single-level assignment (conversion rules live there).
        // (forced != nullptr: place this value as is; nullptr: the real write)
        set_one(slot, keys[0], &holder);
        child = value_slot(slot, keys[0]);
        if (!child) throw std::runtime_error("cannot assign through index " + keys[0].to_string());
    }
    value_set_path(*child, keys + 1, n - 1, set_one);
}


// Returns the SLOT of the value at slot[k0]...[kn-1]; applies copy-on-write along the path and at
// the target itself (n == 0: the root itself). For operations like push/pop that "modify the
// variable in place": the target comes back uniquely owned and the caller modifies it directly.
template <class SetOne>
inline Value& value_path_slot(Value& slot, const Value* keys, int n, SetOne&& set_one) {
    Value* cur = &slot;
    for (int i = 0; i < n; ++i) {
        cur->detach();
        Value* child = value_slot(*cur, keys[i]);
        if (cur->type() == Value::STRUCT) {
            // struct field: used as is (if null the caller reports "not an array")
        } else if (!child) {
            Value holder(std::make_shared<ValueArray>());
            set_one(*cur, keys[i], &holder);
            child = value_slot(*cur, keys[i]);
            if (!child) throw std::runtime_error("cannot reach index " + keys[i].to_string());
        }
        cur = child;
    }
    cur->detach();
    return *cur;
}

// push onto a map is an ERROR (every push path calls this: push(), array::push(), CLI/web builtins).
// OLD: push appended a single keyless value to the end of the map -> the pair alignment shifted,
// keys added later were silently read as null and one value vanished entirely (in 1.0.x the internal
// marker also leaked into the output). The only way to add to a map is $m[$k] = $v.
inline void value_push_not_map(const Value& target, const char* fn) {
    if (target.type() == Value::ARRAY) {
        const auto& v = *target.vec_ptr();
        if (!v.empty() && v[0].is_assoc_marker())
            throw std::runtime_error(std::string(fn) + " cannot add to a map; use $m[$key] = $value");
    }
}

inline bool value_is_map(const Value& v) {
    if (v.type() != Value::ARRAY) return false;
    const auto& a = *v.vec_ptr();
    return !a.empty() && a[0].is_assoc_marker();
}

// List-only operations (pop, sort, slice, chunk, zip, reverse, unique, flatten, join, max/min)
// are an ERROR on a map. OLD: they treated the map's internal flat layout ["<marker>", k0, v0, ...] as a list --
// reverse/join leaked the internal marker into the output, slice/chunk/zip gave wrong pieces, pop lost data
// (the key stayed, the value added later vanished). One rule, one message (fn = "array::sort()").
inline void value_list_only(const Value& v, const std::string& fn) {
    if (value_is_map(v))
        throw std::runtime_error(fn + " takes a list, not a map; use array::values($m) first");
}

// The single definition of push/pop. The target is a uniquely owned slot.
inline Value value_push(Value& target, const Value& v, const char* fn = "push()") {
    if (target.type() != Value::ARRAY) throw std::runtime_error("push() requires array as first argument");
    value_push_not_map(target, fn);
    target.vec_ptr()->push_back(v);
    // Returns the new length. (1.x returned the array itself; under value semantics that
    // makes a second owner of the variable's storage and would force the next push to copy.)
    return Value((int64_t)target.vec_ptr()->size());
}
inline Value value_pop(Value& target) {
    if (target.type() != Value::ARRAY) throw std::runtime_error("pop() requires array");
    value_list_only(target, "pop()");
    auto& vec = *target.vec_ptr();
    if (vec.empty()) return Value();
    Value last = std::move(vec.back());
    vec.pop_back();
    return last;
}
} // namespace look

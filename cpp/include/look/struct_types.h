#pragma once
// Typed struct: name + TYPE + default; a struct is its own VALUE KIND (Value::STRUCT).
//
//   struct User {
//       name  string              # no default -> zero value ("")
//       age   int = 18            # typed + default
//       score ?float              # may be null
//       boss  User                # another struct (may be null)
//       note  any                 # accepts any value
//   }
// Single syntax: `name type [= default]`. An untyped field and v1's `name: default` syntax
// are rejected in the parser (parser.cpp).
//
// LAYOUT (defined in one place):
//   definition (def) = ARRAY, triples [field, default, type]          (declaration order)
//   instance         = Value::STRUCT, slots [name, def, field0, field1, ...]
// Field i's value is in slot 2+i -> access is by INDEX, not by name lookup. A struct is not an
// array: array:: functions, numeric indexing and any code that assumes an array reject it
// (in v1 a struct was internally an assoc array; array::set could bypass the type check).
//
// The rules come from this single header in BOTH engines (two separate implementations = divergence):
//   * the type name must be known (builtin or a declared struct);
//   * a value that does not fit the field -> error (at construction and on later assignment);
//   * int may be written to a float field (converted to float); the reverse and all other mixes are errors;
//   * null may be written only to ?type, any, fn and struct-typed fields;
//   * the shape is FIXED: no undeclared field (neither read nor written).
#include "look/runtime_host.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace look {

constexpr size_t STRUCT_SLOT0 = 2;   // position of the first field in the slot vector

inline const std::string& struct_name(const std::vector<Value>& sv)       { return sv[0].str_ref(); }
inline const std::vector<Value>& struct_def(const std::vector<Value>& sv) { return *sv[1].as_array(); }
inline size_t struct_field_count(const std::vector<Value>& def)           { return def.size() / 3; }

// Type name shown to the user (error messages).
inline std::string struct_value_type_name(const Value& v) {
    switch (v.type()) {
        case Value::NONE:   return "null";
        case Value::INT:    return "int";
        case Value::FLOAT:  return "float";
        case Value::STRING: return "string";
        case Value::BOOL:   return "bool";
        case Value::BYTECODE_FN: return "fn";
        case Value::STRUCT: return struct_name(*v.as_struct());
        case Value::ARRAY: {
            const auto& a = *v.as_array();
            if (!a.empty() && a[0].is_assoc_marker()) return "map";
            return "array";
        }
        default: return "value";
    }
}

inline bool struct_type_is_builtin(const std::string& base) {
    return base == "int" || base == "float" || base == "string" || base == "bool"
        || base == "array" || base == "map" || base == "fn" || base == "any";
}

// Is it a known type name: builtin type or declared struct. `is_struct(name)` comes from the engine.
// Structs may refer forward to each other, so the check happens not at declaration but when a
// struct is constructed (and on the web, in bulk at the end of setup).
template <class IsStruct>
inline void struct_check_type_known(const std::string& sname, const std::string& fname,
                                    const std::string& type, IsStruct&& is_struct) {
    if (type.empty()) return;
    const std::string base = type[0] == '?' ? type.substr(1) : type;
    if (struct_type_is_builtin(base) || is_struct(base)) return;
    throw std::runtime_error("struct '" + sname + "': field '" + fname + "' has unknown type '"
                             + base + "' (expected int, float, string, bool, array, map, fn, any "
                               "or the name of a struct)");
}

// Default value of a typed field when none is declared. array/map are produced fresh for EVERY instance.
inline Value struct_zero_value(const std::string& type) {
    if (type.empty() || type[0] == '?') return Value();
    if (type == "int")    return Value((int64_t)0);
    if (type == "float")  return Value(0.0);
    if (type == "string") return Value(std::string());
    if (type == "bool")   return Value(false);
    if (type == "array")  return Value(std::make_shared<ValueArray>());
    if (type == "map") {
        auto m = std::make_shared<ValueArray>();
        m->push_back(Value::assoc_marker());
        return Value(m);
    }
    return Value();   // any, fn, struct name -> null
}

// The default value is SHARED with the instance (copy-on-write): if the instance writes to a field the write
// path takes its own copy first, and the value in the declaration does not change. (1.0.3 made a deep
// copy here -- arrays were shared by reference back then.)
inline Value struct_instance_default(const Value& def, const std::string& type) {
    if (def.type() == Value::NONE) return struct_zero_value(type);
    return def;
}

// Does the value fit the field type? Throws if not; converts int->float in place.
inline void struct_check_field(const std::string& sname, const std::string& fname,
                               const std::string& type, Value& v) {
    if (type.empty()) return;
    // Fast path (runs on every field write): the common matches finish without building a string.
    switch (v.type()) {
        case Value::INT:    if (type == "int"    || type == "?int")    return; break;
        case Value::STRING: if (type == "string" || type == "?string") return; break;
        case Value::FLOAT:  if (type == "float"  || type == "?float")  return; break;
        case Value::BOOL:   if (type == "bool"   || type == "?bool")   return; break;
        default: break;
    }
    if (type == "any" || type == "?any") return;
    const bool nullable = type[0] == '?';
    const std::string base = nullable ? type.substr(1) : type;
    if (base == "any") return;
    if (v.type() == Value::NONE) {
        if (nullable || base == "fn" || !struct_type_is_builtin(base)) return;
        throw std::runtime_error("struct '" + sname + "': field '" + fname + "' expects "
                                 + base + ", got null (declare it as ?" + base + " to allow null)");
    }
    if (base == "float" && v.type() == Value::INT) { v = Value((double)v.as_int()); return; }
    std::string got = struct_value_type_name(v);
    if (got == base) return;
    // An empty list [] may also be written as a map that has not received a key yet.
    if (base == "map" && got == "array" && v.as_array()->empty()) return;
    throw std::runtime_error("struct '" + sname + "': field '" + fname + "' expects "
                             + type + ", got " + got);
}

[[noreturn]] inline void struct_no_field(const std::string& sname, const std::string& fname) {
    throw std::runtime_error("struct '" + sname + "' has no field '" + fname + "'");
}

// Field name -> field order (0..n-1); -1 if absent. Linear search over the definition: runs only on a
// cache miss; the VM hot path caches the result per instruction.
inline int struct_field_index(const std::vector<Value>& def, const std::string& field) {
    for (size_t j = 0; j + 2 < def.size(); j += 3)
        if (def[j].str_ref() == field) return (int)(j / 3);
    return -1;
}

// New instance: every field with its default (a copy per instance), type names and defaults
// already checked. Literal fields are then placed with struct_set.
template <class IsStruct>
inline Value struct_new(const std::string& sname, const Value& def_value, IsStruct&& is_struct) {
    const auto& def = *def_value.as_array();
    auto sv = std::make_shared<ValueArray>();
    sv->reserve(STRUCT_SLOT0 + struct_field_count(def));
    sv->push_back(Value(sname));
    sv->push_back(def_value);
    for (size_t j = 0; j + 2 < def.size(); j += 3) {
        const std::string& fname = def[j].str_ref();
        const std::string  ftype = def[j + 2].to_string();
        struct_check_type_known(sname, fname, ftype, is_struct);
        Value dv = struct_instance_default(def[j + 1], ftype);
        struct_check_field(sname, fname, ftype, dv);
        sv->push_back(std::move(dv));
    }
    return Value::make_struct(std::move(sv));
}

// Type-checked write to field i.
inline void struct_set(std::vector<Value>& sv, int i, Value v) {
    const auto& def = struct_def(sv);
    // str_ref(): the type name is not copied (to_string allocated a std::string on every write).
    struct_check_field(struct_name(sv), def[(size_t)i * 3].str_ref(), def[(size_t)i * 3 + 2].str_ref(), v);
    sv[STRUCT_SLOT0 + (size_t)i] = std::move(v);
}

// Read/write by name (the VM's uncached path). Unknown field -> error.
inline const Value& struct_get_named(const std::vector<Value>& sv, const std::string& field) {
    int i = struct_field_index(struct_def(sv), field);
    if (i < 0) struct_no_field(struct_name(sv), field);
    return sv[STRUCT_SLOT0 + (size_t)i];
}
inline void struct_set_named(std::vector<Value>& sv, const std::string& field, Value v) {
    int i = struct_field_index(struct_def(sv), field);
    if (i < 0) struct_no_field(struct_name(sv), field);
    struct_set(sv, i, std::move(v));
}

// Returns the struct's fields as a map (["__assoc__", name, value, ...]) -- for NON-HOT consumers
// such as JSON, templates and foreach. Values are shared (not copied).
inline Value struct_to_map(const Value& s) {
    const auto& sv = *s.as_struct();
    const auto& def = struct_def(sv);
    auto m = std::make_shared<ValueArray>();
    m->reserve(1 + 2 * struct_field_count(def));
    m->push_back(Value::assoc_marker());
    for (size_t j = 0, i = 0; j + 2 < def.size(); j += 3, ++i) {
        m->push_back(def[j]);
        m->push_back(sv[STRUCT_SLOT0 + i]);
    }
    return Value(m);
}

} // namespace look

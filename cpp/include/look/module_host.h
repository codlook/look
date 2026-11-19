#pragma once
// Everything the built-in modules need from the runtime — and only this much.
//
// The module factories (make_stdlib, make_extra_stdlib, make_web_modules, …) used to take
// the interpreter object (today's RuntimeHost); yet the modules use only these four things from it. When moving
// to a single engine (the VM), the dependency was reduced to this interface so that the module table
// can be built without the interpreter object. Today RuntimeHost implements it; when the VM brings
// its own host the modules need not be touched.
#include <vector>

namespace look {

class Value;   // value.h (forward declaration — so that this header can be included cheaply from anywhere)

class ModuleHost {
public:
    virtual ~ModuleHost() = default;

    // Calls a LOOK function value (array::map/filter/…, db::transaction, http::stream
    // callbacks). If the value is a VM closure it runs in the active VM.
    virtual Value invoke(const Value& fn, std::vector<Value> args) = 0;

    // Is top-level setup in progress — db::query handles the side effect during setup accordingly.
    virtual bool is_setup_mode() const = 0;

    // runtime::stats counters.
    virtual int  get_route_count()   const = 0;
    virtual long get_uptime_sec()    const = 0;
};

} // namespace look

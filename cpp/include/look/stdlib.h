#pragma once

#include "look/runtime_host.h"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace look {

struct WebContext;

std::map<std::string, Module> make_stdlib(ModuleHost* interp);
std::map<std::string, Module> make_web_modules(WebContext* ctx, ModuleHost* interp);
std::map<std::string, Module> make_extra_stdlib(ModuleHost* interp);
Module make_file_module();
Module make_date_module();
Module make_template_module(ModuleHost* interp);
// The operating system's cryptographic random source (urandom / BCrypt); throws if it cannot be read.
std::vector<uint8_t> secure_random_bytes(size_t n);
// Default CSRF gate (web_stdlib.cpp): the LOOK_SECRET problem ("" = ok) and the request check (reason goes into why).
std::string csrf_secret_problem();
bool csrf_request_ok(WebContext* ctx, std::string& why);
// The SAME encoder as json::encode (web_stdlib.cpp) — everything that writes JSON, such as http::post_json, uses it.
std::string json_encode_value(const Value& v);

} // namespace look

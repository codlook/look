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
// İşletim sisteminin kriptografik rastgele kaynağı (urandom / BCrypt); okunamazsa fırlatır.
std::vector<uint8_t> secure_random_bytes(size_t n);
// Varsayılan CSRF kapısı (web_stdlib.cpp): LOOK_SECRET sorunu ("" = tamam) ve istek denetimi (neden why'a).
std::string csrf_secret_problem();
bool csrf_request_ok(WebContext* ctx, std::string& why);

} // namespace look

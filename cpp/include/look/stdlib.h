#pragma once

#include "look/runtime_host.h"
#include <map>
#include <string>

namespace look {

struct WebContext;

std::map<std::string, Module> make_stdlib(ModuleHost* interp);
std::map<std::string, Module> make_web_modules(WebContext* ctx, ModuleHost* interp);
std::map<std::string, Module> make_extra_stdlib(ModuleHost* interp);
Module make_file_module();
Module make_date_module();
Module make_template_module(ModuleHost* interp);

} // namespace look

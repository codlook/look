#pragma once

#include "look/interpreter.h"
#include <map>
#include <string>

namespace look {

struct WebContext;

std::map<std::string, Module> make_stdlib();
std::map<std::string, Module> make_web_modules(WebContext* ctx);
std::map<std::string, Module> make_extra_stdlib(Interpreter* interp);
Module make_file_module();
Module make_date_module();
Module make_template_module(Interpreter* interp);

} // namespace look

#pragma once
#include <string>

namespace look {

// lk install <pkg> — GitHub package installer
// pkg format: "github.com/user/repo[@ref]"
int cmd_install(const std::string& pkg, bool verbose);

// lk install (no args) — re-install all packages from look.lock
int cmd_install_all(bool verbose);

// lk module install <name> — install official module from codlook/look-modules
// Downloads <name>/<name>.lk → ~/.look/modules/<name>/
int cmd_module_install(const std::string& name, bool verbose);

// lk module list — list installed modules
int cmd_module_list();

} // namespace look

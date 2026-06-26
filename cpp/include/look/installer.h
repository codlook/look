#pragma once
#include <string>

namespace look {

// look install <pkg> — GitHub package installer
// pkg format: "github.com/user/repo[@ref]"
//   github.com/ali/look-stripe        → downloads main branch
//   github.com/ali/look-stripe@v1.2   → downloads tag v1.2
//
// Output: pkg/ali/look-stripe/
// Lock:   look.lock
int cmd_install(const std::string& pkg, bool verbose);

// look install (no args) — re-install all packages from look.lock
int cmd_install_all(bool verbose);

} // namespace look

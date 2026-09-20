#pragma once
#include <string>

namespace look {

// lk install <pkg> — GitHub package installer
// pkg format: "github.com/user/repo[@ref]"
// locked=true: install the exact commit recorded in look.lock (reproducible +
// tamper-evident); errors if there is no locked sha for the package.
int cmd_install(const std::string& pkg, bool verbose, bool locked = false);

// lk install (no args) — re-install all packages from look.lock (pinned to sha).
// locked=true: strict — fail if any lock entry lacks a sha (no ref fallback).
int cmd_install_all(bool verbose, bool locked = false);

// lk module install <github.com/user/repo[@ref]> — install module from any GitHub repo
// Extracts to ~/.look/modules/<repo>/   usage: use <repo>;
int cmd_module_install(const std::string& pkg_url, bool verbose);

// lk module list — fetch official module list from codlook/look-modules (GitHub API)
int cmd_module_list();

} // namespace look

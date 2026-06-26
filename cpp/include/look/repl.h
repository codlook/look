#pragma once
namespace look {
// Entry point for `look repl` subcommand. Never returns normally (loops until :exit/Ctrl+C).
int run_repl();
} // namespace look

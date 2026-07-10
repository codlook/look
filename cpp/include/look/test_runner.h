#pragma once
#include <string>
#include <stdexcept>

namespace look {

// Thrown by assert:: functions to signal a test failure (distinct from LookRuntimeError)
struct TestAssertionError : std::runtime_error {
    explicit TestAssertionError(const std::string& msg) : std::runtime_error(msg) {}
};

// Entry point — called by main.cpp when `look test [pattern] [--verbose]`
// Returns exit code: 0 = all pass, 1 = failures or errors
int run_test_mode(const std::string& pattern, bool verbose);

} // namespace look

#pragma once
// lk fmt — LOOK source formatter (gofmt model: NO OPTIONS, a single canonical form).
//
// V1 = re-indent: indent = brace/bracket/parenthesis depth × 4; trailing
// whitespace is trimmed; consecutive blank lines are reduced to one. Does NOT touch token CONTENT
// (only leading/trailing whitespace) → meaning is preserved by construction. Comments are preserved (char-based).
//
// GUARANTEE: the formatted output is re-lexed and its non-trivia token sequence is
// compared with the input; if it does NOT match formatting is CANCELLED (the file is unchanged). In edge cases
// (multi-line strings etc.) this gives a safe-fallback guarantee instead of corruption.
#include <string>
#include <vector>

namespace look {

// Bring the source to canonical form. Returns formatted if the meaning is unchanged; if string tracking trips on an
// edge case (verify fails) it returns the original UNCHANGED (reports it with ok=false).
std::string look_format_source(const std::string& src, bool* ok = nullptr);

// lk fmt CLI. If files is empty stdin. check=true → CI mode: exit 1 if an unformatted file exists (does NOT write).
// Otherwise formats files in place. Returns: 0 clean, 1 unformatted in check, 2 error.
int run_fmt(const std::vector<std::string>& files, bool check);

} // namespace look

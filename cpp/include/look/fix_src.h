#pragma once
// lk fix — converts deprecated spellings to the new spelling (gofix model, NO OPTIONS).
//
//   push(X, v)        (statement) → X = array::push(X, v)
//   pop(X)            (statement) → X = array::slice(X, 0, -1)
//   $v = pop(X)       (statement) → $v = X[-1]  +  on a new line  X = array::slice(X, 0, -1)
//   array::pop(X)     (anywhere)  → X[-1]
//
// X must be a variable or an index/member path ($l, $m["k"], $u.tags). Every other use (push/pop
// inside a larger expression) is NOT CHANGED and is reported with the reason.
// The rewrite is done by SOURCE RANGES (token positions): untouched lines, comments
// and formatting stay as they are. The result is re-parsed; if it does not parse the file is unchanged.
#include <string>
#include <vector>

namespace look {

struct FixResult {
    std::string output;                 // rewritten source (the input if there is an error)
    // Each line has the form "<line>: ..."; the CLI prepends the file name.
    std::vector<std::string> changes;   // "<line>: old  ->  new"
    std::vector<std::string> notes;     // "<line>: NOTE: ..." (behaviour difference on an empty list)
    std::vector<std::string> skipped;   // "<line>: not rewritten: reason"
    std::string error;                  // if not empty the file could not be processed (output = input)
};

FixResult look_fix_source(const std::string& src);

// lk fix CLI. If files is empty stdin → stdout. check=true → does not write, lists what would change.
// Returns: 0 clean; 1 something would change (check) or a call that cannot be rewritten remains; 2 error.
int run_fix(const std::vector<std::string>& files, bool check);

} // namespace look

#pragma once
// Zip-Slip containment decision — pure predicate ("seam opened" out of installer.cpp). Tested by table
// WITHOUT real extraction/miniz; locks the regression of the security decision without a network.
//
// Will a zip entry (filename) be extracted INSIDE dest_dir? The normalized path must stay
// within dest_dir. A PLAIN string prefix is NOT ENOUGH: the prefix "/pkg/user/repo" also
// matches "/pkg/user/repo-evil" (sibling-prefix escape → write OUTSIDE dest). Prefix + SEPARATOR-BOUNDARY is required.
#include <string>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace look {

// ZIP bomb decision: the number of entries in the archive's central directory and their declared uncompressed sizes.
// While extracting, miniz checks the real size against the declared one (any excess is an error), so the declared total
// is the real upper bound. Empty return = accept; non-empty = the English reason to show the user.
inline constexpr uint64_t ZIP_MAX_TOTAL   = 256ull * 1024 * 1024;   // packages are source code
inline constexpr size_t   ZIP_MAX_ENTRIES = 20000;
inline std::string zip_size_problem(const std::vector<uint64_t>& uncomp_sizes) {
    if (uncomp_sizes.size() > ZIP_MAX_ENTRIES)
        return "the archive has more than " + std::to_string(ZIP_MAX_ENTRIES) + " entries";
    uint64_t total = 0;
    for (uint64_t s : uncomp_sizes) {
        if (s > ZIP_MAX_TOTAL - total)   // overflow-safe total
            return "the archive unpacks to more than " + std::to_string(ZIP_MAX_TOTAL >> 20) + " MB";
        total += s;
    }
    return "";
}

inline bool zip_entry_inside_dest(const std::string& filename,
                                  const std::filesystem::path& dest_dir) {
    namespace fs = std::filesystem;
    fs::path safe_dest = fs::weakly_canonical(dest_dir);
    fs::path out_path  = fs::weakly_canonical(dest_dir / filename);
    const std::string dest_str = safe_dest.string();
    const std::string out_str  = out_path.string();
    return out_str.size() >= dest_str.size() &&
           out_str.compare(0, dest_str.size(), dest_str) == 0 &&
           (out_str.size() == dest_str.size() ||
            out_str[dest_str.size()] == '/' ||
            out_str[dest_str.size()] == '\\');
}

} // namespace look

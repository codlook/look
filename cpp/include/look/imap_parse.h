#pragma once
// IMAP pure parsers — "unstitch" (the pg_parse.h / smtp_parse.h pattern): separate the network-fed,
// PRE-AUTH parse logic from I/O → a single inline definition (no drift), fuzz + table test.
// imap_server.cpp includes this header; there is NO separate .static definition.
#include <string>
#include <cctype>

namespace look {

// "tag SP command SP args" → tag, cmd (upper), args (the rest). A line without spaces → all of it is the tag.
inline void imap_parse_command(const std::string& line, std::string& tag,
                               std::string& cmd, std::string& args) {
    tag.clear(); cmd.clear(); args.clear();
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) { tag = line; return; }
    tag = line.substr(0, sp1);
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) { cmd = line.substr(sp1 + 1); }
    else { cmd = line.substr(sp1 + 1, sp2 - sp1 - 1); args = line.substr(sp2 + 1); }
    for (char& ch : cmd) ch = (char)std::toupper((unsigned char)ch);  // the command is case-insensitive
}

// Extract two arguments (LOGIN "user" "pass" or LOGIN user pass) — if quoted, up to the closing quote.
inline void imap_split_two(const std::string& args, std::string& a, std::string& b) {
    a.clear(); b.clear();
    auto unquote = [](std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
        return s;
    };
    size_t i = 0;
    auto next_token = [&](std::string& tok) {
        while (i < args.size() && args[i] == ' ') i++;
        if (i >= args.size()) return false;
        if (args[i] == '"') {
            size_t e = args.find('"', i + 1);
            if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
            else { tok = args.substr(i, e - i + 1); i = e + 1; }
        } else {
            size_t e = args.find(' ', i);
            if (e == std::string::npos) { tok = args.substr(i); i = args.size(); }
            else { tok = args.substr(i, e - i); i = e; }
        }
        return true;
    };
    std::string t1, t2;
    if (next_token(t1)) a = unquote(t1);
    if (next_token(t2)) b = unquote(t2);
}

// SECURITY: PURE (no I/O) validation of the mailbox name — a path traversal + injection guard.
// The string gate that comes BEFORE resolve_mailbox's fs-canonical containment. `cleaned` returns the
// unquoted name; INBOX is special (the caller returns the root). A returned false = reject.
//   Documented past bug: `INBOX; rm -rf /` was ACCEPTED — the checks only looked at \0/absolute/'..',
//   an INNER separator+`;`+space passed. A mailbox name is a PATH COMPONENT: it cannot contain a separator
//   or a control character. (fs-containment stays in the .cpp — it needs a real path.)
// Returns: true + cleaned="INBOX" in the special case; true + clean name; false = invalid/traversal.
inline bool imap_mailbox_name_ok(const std::string& name, std::string& cleaned) {
    cleaned.clear();
    std::string mb = name;
    if (mb.size() >= 2 && mb.front() == '"' && mb.back() == '"') mb = mb.substr(1, mb.size() - 2);
    if (mb.empty() || mb == "INBOX") { cleaned = "INBOX"; return true; }   // INBOX = Maildir root
    if (mb.find('\0') != std::string::npos) return false;
    if (mb.front() == '/' || mb.front() == '\\') return false;
    if (mb.find("..") != std::string::npos) return false;
    for (unsigned char c : mb)
        if (c == '/' || c == '\\' || c == ';' || c < 0x20 || c == 0x7F) return false;
    cleaned = mb;
    return true;
}

} // namespace look

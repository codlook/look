// The installer's redirect rule (look/url_safe.h redirect_allowed) against Go's net/url as the referee.
// stdin: lines "<url>\t<ALLOW|DENY|ERR>\t<host Go reads>" made by input_fuzz/url_referee.go
// (ALLOW = Go reads an https URL whose host is GitHub's; DENY = Go reads another host; ERR = Go cannot parse it).
//   * LOOK allows a URL whose host Go reads as another one  -> FAIL: a redirect to a host that is not GitHub's
//     would be followed (for example "https://github.com:x@evil.com/": the user name is "github.com:x");
//   * LOOK allows a URL Go cannot parse                     -> FAIL: two readers of one text disagree;
//   * LOOK denies what Go allows                            -> counted, a few listed: stricter than Go (safe).
// Build: g++ -std=c++17 -Iinclude tests/url_redirect_referee_test.cpp -o /tmp/urt
#include "look/url_safe.h"
#include <cstdio>
#include <iostream>
#include <string>

int main() {
    std::string line;
    int total = 0, looser = 0, unparsed = 0, stricter = 0, agree_allow = 0;
    while (std::getline(std::cin, line)) {
        size_t t1 = line.find('\t'); if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1); if (t2 == std::string::npos) continue;
        std::string url = line.substr(0, t1), go = line.substr(t1 + 1, t2 - t1 - 1), gohost = line.substr(t2 + 1);
        std::string err;
        bool lk = look::redirect_allowed(url, err);
        ++total;
        if (lk && go == "DENY") {
            if (++looser <= 8) std::printf("  LOOSER   %-50s LOOK allows; Go reads host \"%s\"\n", url.c_str(), gohost.c_str());
        } else if (lk && go == "ERR") {
            if (++unparsed <= 6) std::printf("  UNPARSED %-50s LOOK allows; Go cannot parse it\n", url.c_str());
        } else if (!lk && go == "ALLOW") {
            if (++stricter <= 4) std::printf("  stricter %-50s LOOK denies; Go allows (host \"%s\")\n", url.c_str(), gohost.c_str());
        } else if (lk) {
            ++agree_allow;
        }
    }
    std::printf("%d URLs: both allow %d, LOOK allows another host than Go reads %d, LOOK allows what Go cannot parse %d, "
                "LOOK stricter than Go %d\n", total, agree_allow, looser, unparsed, stricter);
    if (looser || unparsed) {
        std::printf("FAIL: redirect_allowed accepts URLs that Go does not read as GitHub's\n");
        return 1;
    }
    std::printf("PASS: redirect_allowed is not looser than Go's net/url\n");
    return 0;
}

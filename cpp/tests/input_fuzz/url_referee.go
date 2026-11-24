// URL referee for url_redirect_referee_test.sh: Go's net/url decides which host a URL names.
// Prints random URLs (fixed seed) with Go's decision for the installer's redirect rule:
//   <url>\t<ALLOW|DENY>\t<host Go reads>
// ERR = Go cannot parse the URL. ALLOW = Go parses the URL, the scheme is https, and Hostname() is github.com, api.github.com, or ends in
// .github.com / .githubusercontent.com. The test runs LOOK's redirect_allowed on the same lines.
// Run: url_referee <cases> <seed>
package main

import (
	"fmt"
	"math/rand"
	"net/url"
	"os"
	"strconv"
	"strings"
)

var schemes = []string{"https://", "https://", "https://", "http://", "HTTPS://", "https:/", "https:", "//", ""}
var users = []string{"", "", "", "github.com@", "github.com:x@", "x@", "x:y@", "api.github.com:443@", "%67ithub.com@", "a.github.com:@"}
var hosts = []string{"github.com", "api.github.com", "codeload.github.com", "objects.githubusercontent.com", "evil.com",
	"github.com.evil.com", "evilgithub.com", "evil.com.github.com", "GITHUB.com", "github.com.", "[::1]", "[github.com]", "", "github.com%2eevil.com", "gith\u0075b.com"}
var ports = []string{"", "", "", ":443", ":80", ":", ":x", ":99999", ":0443"}
var seps = []string{"", "/", "/", "?", "#", "\\", ";", " ", "\t", "%00", "/x/y", "?a=.github.com", "#.github.com", "?@github.com", "#@github.com", "/@github.com", "\\@github.com", "/..;/"}
var tails = []string{"", "", "", "/a", "?x=1", "#f", "@evil.com", "@github.com", ".github.com", ":443", "?.github.com", "#.githubusercontent.com"}

func pick(r *rand.Rand, s []string) string { return s[r.Intn(len(s))] }

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	seed, _ := strconv.ParseInt(os.Args[2], 10, 64)
	r := rand.New(rand.NewSource(seed))
	seen := map[string]bool{}
	for len(seen) < n {
		s := pick(r, schemes) + pick(r, users) + pick(r, hosts) + pick(r, ports) + pick(r, seps) + pick(r, tails)
		if r.Intn(4) == 0 {
			s += pick(r, seps) + pick(r, tails)
		}
		if seen[s] || strings.ContainsAny(s, "\n\r") {
			continue
		}
		seen[s] = true
		dec, host := "ERR", ""
		if u, err := url.Parse(s); err == nil {
			dec = "DENY"
			host = u.Hostname()
			if u.Scheme == "https" && (host == "github.com" || host == "api.github.com" ||
				strings.HasSuffix(host, ".github.com") || strings.HasSuffix(host, ".githubusercontent.com")) {
				dec = "ALLOW"
			}
		}
		fmt.Printf("%s\t%s\t%s\n", s, dec, host)
	}
}

// URL referee for url_parse_referee_test.sh: Go's net/url says what http::get's URL names.
// Prints random URLs (fixed seed), every text field in hex so that any byte can travel:
//   <url>\t<status>\t<tls 0|1>\t<host>\t<port>\t<user>\t<has-userinfo 0|1>\t<pass>
// status: ERR (Go cannot parse the URL), OTHER (an http or https URL is not what it names), OK.
// OK: scheme http or https, Hostname() not empty. port is Go's Port() or the scheme's default.
// The test runs look::parse_url on the same lines (look/url_parse.h) and compares.
// Run: url_parse_referee <cases> <seed>
package main

import (
	"encoding/hex"
	"fmt"
	"math/rand"
	"net/url"
	"os"
	"strconv"
	"strings"
)

var schemes = []string{"http://", "http://", "https://", "https://", "HTTP://", "Https://", "ftp://", "http:/", "http:", "//", "", "http:\\\\", "http://http://"}
var users = []string{"", "", "", "", "u@", "u:p@", "u:@", ":p@", "@", ":@", "a@b@", "u:p:q@", "u%40x:p%3Ay@", "%@", "u:p%zz@", "github.com:x@", "us er@", "u\x00@"}
var hosts = []string{"example.com", "example.com", "example.com", "127.0.0.1", "0x7f.1", "2130706433", "017700000001", "127.1", "0177.0.0.1",
	"[::1]", "[::1", "::1]", "::1", "[fe80::1%25eth0]", "[fe80::1%eth0]", "[::ffff:127.0.0.1]", "[]", "[v1.x]", "localhost", "localhost.", "exa mple.com",
	"ex\x00ample.com", "good.com\x00.evil.com", "EXAMPLE.com", "%65xample.com", "%2e", "xn--n3h.com", "日本.jp", "a_b.com", "a$b.com", "a;b.com", "a'b.com",
	"", "a b", "a\tb", "a<b", "a{b}", "a|b", "a^b", "a`b", "a\"b", "a,b", "a!b", "a*b", "a(b)", "a=b", "a+b", "a~b", "-a.com", "a-.com", ".", "a..b", "1.2.3", "999.1.1.1", "evil.com\\@good.com", "evil.com\\good.com"}
var ports = []string{"", "", "", ":80", ":443", ":8080", ":", ":0", ":00080", ":65535", ":65536", ":99999", ":-1", ":+1", ":x", ":80x", ": 80", ":0x50", ":１２"}
var seps = []string{"", "/", "/", "/", "?", "#", "\\", ";", " ", "\t", "%00", "/a/b", "/a?b=c#d", "?a=b", "#f", "/a b", "/\x00", "/%zz", "/%41", "/a#b#c", "?a?b", "/..", "/./"}
var tails = []string{"", "", "", "?x=1", "#f", "@evil.com", "@good.com", ":443", "/@x", "?@x", "#@x", "\\@x"}

func pick(r *rand.Rand, s []string) string { return s[r.Intn(len(s))] }
func h(s string) string                    { return hex.EncodeToString([]byte(s)) }

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	seed, _ := strconv.ParseInt(os.Args[2], 10, 64)
	r := rand.New(rand.NewSource(seed))
	seen := map[string]bool{}
	for len(seen) < n {
		s := pick(r, schemes) + pick(r, users) + pick(r, hosts) + pick(r, ports) + pick(r, seps) + pick(r, tails)
		if r.Intn(3) == 0 {
			s += pick(r, seps) + pick(r, tails)
		}
		if seen[s] || strings.ContainsAny(s, "\n\r") {
			continue
		}
		seen[s] = true
		status, tls, host, port, user, hasUser, pass := "ERR", "0", "", "", "", "0", ""
		if u, err := url.Parse(s); err == nil {
			status = "OTHER"
			sch := strings.ToLower(u.Scheme)
			if (sch == "http" || sch == "https") && u.Hostname() != "" {
				status = "OK"
				host = u.Hostname()
				port = u.Port()
				if port == "" {
					port = "80"
					if sch == "https" {
						port = "443"
					}
				}
				if sch == "https" {
					tls = "1"
				}
				if u.User != nil {
					hasUser = "1"
					user = u.User.Username()
					pass, _ = u.User.Password()
				}
			}
		}
		fmt.Printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", h(s), status, tls, h(host), port, h(user), hasUser, h(pass))
	}
}

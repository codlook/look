# LOOK Security Regression Tests

26 verified vulnerabilities — each must be **rejected** by the runtime.

## Quick Start

```bash
look test tests/security/regression.lk
```

Expected output: `26 passed, 0 failed`

---

## Test Map

| # | Test name | Vulnerability | Fixed in |
|---|-----------|--------------|----------|
| 1 | session: path traversal via ../ | Cookie value used as file path without sanitization | v1.2.0 |
| 2 | session: null byte in session id | Null byte truncation in file path | v1.2.0 |
| 3 | json: deeply nested array | Unbounded recursion → C++ stack overflow | v1.2.0 |
| 4 | json: deeply nested object | Same as above, object variant | v1.2.0 |
| 5 | json: string ending with bare backslash | OOB read past end of string buffer | v1.3.1 |
| 6 | websocket: oversized frame > 16 MB | 64-bit plen with no upper bound → 8 EB allocation | v1.3.1 |
| 7 | ssrf: loopback IPv4 | `http://127.0.0.1` reaches internal services | v1.2.0 |
| 8 | ssrf: localhost | DNS resolves to 127.0.0.1 | v1.2.0 |
| 9 | ssrf: RFC1918 10.x.x.x | Private range reachable | v1.2.0 |
| 10 | ssrf: RFC1918 192.168.x.x | Private range reachable | v1.2.0 |
| 11 | ssrf: RFC1918 172.16.x.x | Private range reachable | v1.2.0 |
| 12 | ssrf: cloud metadata 169.254.x.x | AWS/GCP/Azure IMDS exposed | v1.2.0 |
| 13 | ssrf: IPv4-mapped IPv6 ::ffff:127.0.0.1 | IPv4 block bypassed via IPv6 notation | v1.3.1 |
| 14 | ssrf: IPv6 unspecified ::/128 | Unspecified address not blocked | v1.3.1 |
| 15 | sql: OR injection | `bind_params()` string escape, not real prepared stmt | v1.3.0 |
| 16 | sql: UNION injection | Same root cause | v1.3.0 |
| 17 | sql: DROP TABLE injection | Same root cause | v1.3.0 |
| 18 | svg: \<script\> tag | Blacklist-based filter, `<script>` not stripped | v1.2.1 |
| 19 | svg: javascript: href | `href="javascript:..."` not stripped | v1.2.1 |
| 20 | svg: \<foreignObject\> | Allowed element enabling iframe injection | v1.2.1 |
| 21 | svg: \<animate values=javascript:\> | Animation attribute carrying JS | v1.2.1 |
| 22 | svg: style url(javascript:) | CSS url() vector not in original blacklist | v1.2.1 |
| 23 | svg: \<use href=data:\> | Data URI SVG nesting | v1.2.1 |
| 24 | string::repeat DoS | No size limit → OOM with n=999999999 | v1.3.1 |
| 25 | channel(-1) infinite capacity | Negative int cast to size_t max → unbounded queue | v1.3.1 |
| 26 | redos: catastrophic backtracking | `std::regex` + adversarial pattern → CPU exhaustion | v1.2.1 |

> Tests #27–28 (template LFI) are included in the file but numbered separately
> because they were discovered in the same audit round as #5–#6.

---

## What "rejected" means

Each test asserts that the runtime either:

- **throws** an exception (the common case — `assert::throws`)
- **returns safe output** with the dangerous content removed (SVG sanitizer tests)
- **returns zero rows** (SQL injection tests)

A test going red means the protection was removed or bypassed.

---

## Running with sanitizers

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -B build_asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build_asan
./build_asan/look test tests/security/regression.lk

# ThreadSanitizer
cmake -B build_tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build_tsan
./build_tsan/look test tests/security/regression.lk
```

Any sanitizer finding on these tests is a **critical** regression.

---

## Adding a new test

1. Find the right group or create one if it is a new category.
2. Write a `test("category: what is rejected", fn() { assert::throws(...) })` block.
3. Add a row to the table above with the CVE/audit reference and the fix version.
4. The CI pipeline picks it up automatically — no configuration needed.

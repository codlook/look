# Dogfooding friction log — Task Manager (B)

**Rule:** I write the app by reading the DOCS (codlook.com/docs = docs/index.html), not the source.
When the docs are not enough, that is a FINDING (what a real user would hit). Every place I got stuck,
every workaround, every doc I couldn't find goes here. The goal is not a working app — it is to measure
friction.

Scope (vertical slice, 5 screens):
- [ ] Register + login (auth::, session::)
- [ ] List page (db::query + template, pagination)
- [ ] Add/edit form (validator::, POST flow, error display)
- [ ] Delete (CSRF/confirm)
- [ ] JSON endpoint (assoc round-trip)

---

## Findings

<!-- Each finding: [LAYER] what I tried → what happened → what the docs said → workaround/fix -->

### FINDING #14 — route::group is a NARROW BAND: not worth the churn in dogfood (refactor = VALIDATION, not a demo)
**[DX + DECISION]** After route::group was added: is converting dogfood from before_route → route::group
better? I did the refactor as a measurement (not a demo). Result: **roughly even, not worth the churn.**
- `""` path: inside route::group("/tasks", ...), /tasks itself becomes `route("GET", "", ...)` — less
  readable than an explicit "/tasks". +1 indentation level, larger diff.
- **COVERAGE DIFFERENCE (the real finding):** before_route runs on every request (including unmatched
  paths → an unauthenticated /tasks/nonexistent gives **302 /login**). Group middleware runs only on a
  matched route → /tasks/nonexistent gives **404** (without auth). Measured (both engines). This is NOT
  a SECURITY hole (route patterns are a static public surface, not secret; no per-record leak — the mw
  runs before "does the task exist?"). For a uniform auth boundary, before_route is slightly more
  conservative → no reason to churn, the existing solution is already in place.
- **IMPORTANT SELF-CORRECTION:** the first draft claimed "route::group MULTIPLIES the use boilerplate" —
  WRONG. In a scratch test I copied dogfood's (redundant) `use ($conn)` pattern and mistook it for a
  measurement. When measured (TEST A/B, both engines): a closure sees `$conn` WITHOUT `use` → a group
  does not force use-doubling. LESSON: hand-written hypothesis code is NOT a measurement; separate it
  with "is this actually the case?" (3rd time this session: jwt_verify signature, before_route example,
  the use claim — all confident prose, none measured).
**Conclusion:** route::group's sweet spot is CLEAN APIs (many routes, shared mw, light bodies, zero use —
see the docs example); in server-rendered CRUD, before_route + inline is already clean. The feature
exists, but not everywhere.

### FINDING #13 — Group protection: the CAPABILITY existed, the DOCS didn't (an external critique thought it was "missing")
**[DOCS + DX]** An external critique: *"LOOK has no global interceptor/middleware chain; as the project
grows you write `jwt_verify` by hand at the head of 50 routes, which sabotages the no-framework
advantage."* **Measurement disproved half of it:** middleware already exists at TWO levels — `before_route()`
(global) + `route(m, p, [mw], fn)` (route-level), in stdlib, including chain-cutting with `stop()`. The
only missing thing is a syntactic `route::group()` — and even that isn't needed: a single `before_route`
+ `request::path()` prefix check covers a whole section.
**The real repetition measured in this app:** `if (!require_login()) { return }` — by hand in **9 handlers**.
Converted to a prefix guard → **9 → 1** (`before_route` protects the `/tasks` prefix). Proven end to end
(lk-fcgi `--mode http`, Docker): `/tasks` & `/tasks/new` unauthenticated → 302 `/login`; logged in → 200;
public (`/login`, `/register`) → 200; VM dispatch clean (no fallback).
**The real finding (source of friction):** not capability, but **visibility**. The docs middleware section
only showed a route-level example; the "protect all of `/admin/*` in one place" pattern and the "why no
`route::group`" decision were NOT WRITTEN → the critique grew out of exactly that gap. **Fix:** added a
"Protecting a whole section — prefix guards" subsection + a design-decision callout to the docs (a
router-DSL layer is deliberately absent: the shortest path, nothing hidden). No code changed — only docs
+ dogfood refactor.

### FINDING #5 — Published release assets are 30 commits behind HEAD (deployment dogfooding, without touching the VPS)

**Layer:** distribution / release process / source↔artifact drift
**What I did:** To deploy the task manager like a real user, I followed the README "Install (prebuilt)"
path: from Releases, `look-lang-linux-1.0.0.zip` → `sudo bash install.sh`.
**What happened (MEASURED, corrected):** The v1.0 release assets DO exist (the docs point correctly).
The release *tag* date is 2026-07-09 but the **assets were refreshed to `5465074` (2026-08-02)** — so NOT
"a month behind". Measurement: `git rev-list --count 5465074..HEAD` = **30 commits / 2 days** (HEAD=e47b80b,
Aug 4). My first "~1 month behind" was WRONG (I confused the release tag date with the asset date) — this
session's "don't claim without measuring" rule was applied to the finding text too, and corrected.
**Conclusion (the gist holds):** session::has (18bf8f4) is INSIDE that 30-commit gap → a user following
the README today downloads a binary WITHOUT session::has → writing the docs' auth example gives a 500.
The doc-guided install path hands you a binary that cannot run the doc's feature.
**Root:** Release asset updates are manual; not on every HEAD. Version fixed at 1.0.0. Memory "release
gate drift". → the real justification for release automation (queued AFTER the deployment round).
**Deploy decision:** build from source on the VPS (a documented but never-tested path; no docker needed).
Constraint: do NOT touch the live `look-test-codlook-com` — a separate dir `/opt/dogfood` + a separate
port `:7700`, `/usr/bin/lk-fcgi` unchanged, the existing service undisturbed.

### FINDING #6 — Build from source: AlmaLinux 8's default gcc can't do C++23, and the README doesn't say so

**Layer:** distribution / build-from-source / missing documented prerequisite
**What I did:** README:215 "Build from source": `cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release`.
Requirement: "C++23 compiler + CMake 3.20+".
**What happened:** AlmaLinux 8.10 default `g++ 8.5.0` → `-std=c++23` **"unrecognized command line option"**.
The Tier-1 platform's default compiler can't do C++23. The fix is `gcc-toolset-12/13`
(`source /opt/rh/gcc-toolset-12/enable`) but the README does NOT SAY this (the info is only in CLAUDE.md).
On a fresh AlmaLinux you also need `dnf install gcc-toolset-12`. Verified: C++23 OK with gcc-toolset-12.
**Fix:** add an "AlmaLinux/RHEL 8: `dnf install gcc-toolset-12` + enable" prerequisite to the
README/deployment docs.

### FINDING #7 — The README's primary build command DOESN'T WORK out of the box (static SSL default ON)

**Layer:** distribution / build-from-source / CMake default
**What happened:** `cmake -S cpp -B cpp/build` (README:215 verbatim) → **"Could NOT find OpenSSL (missing:
OPENSSL_CRYPTO_LIBRARY)"** — even though `openssl-devel` is INSTALLED. Root: `CMakeLists:210
LOOK_STATIC_SSL=ON` (default) → `OPENSSL_USE_STATIC_LIBS TRUE` → it looks for `libcrypto.a`; `openssl-devel`
only ships `.so`, no `.a`. README:231 says in fine print "install static OpenSSL, or configure with..."
but **the primary/copy-pasted command is broken**. Fix: `dnf install openssl-static` OR
`-DLOOK_STATIC_SSL=OFF` (dynamic — sufficient for VPS deploy). With the latter, build OK (70s, lk-fcgi 4.3MB).
**Fix:** the README's primary command should either include `-DLOOK_STATIC_SSL=OFF` or state the
static-libs prerequisite clearly. "Copy-paste should just work" — right now it blows up on first try.

### FINDING #8 — Secure session cookie + plain HTTP → session breaks in older curl

**Layer:** session / cookie / --mode http direct scenario
**What happened:** Testing the app on :7700 (plain http) on the VPS, registration gave **419 (CSRF)** —
the session wasn't kept between GET/POST. Root: the session cookie is `Secure` (`Set-Cookie: ...; HttpOnly;
Secure; SameSite=Lax`); **curl 7.61.1 (RHEL8) does not send a Secure cookie over plain http** → each
request gets a new SID → csrf mismatch. (Local curl 8.x sends Secure to localhost → it worked locally; a
version difference.) Passing the cookie manually with `-H "Cookie:"` made the FULL flow work
(register/task/JSON, 0 fallback).
**Assessment:** NOT a problem in production (test.codlook.com is behind HTTPS/Apache → the browser gets the
cookie over HTTPS). BUT for someone using `--mode http` DIRECTLY (no TLS, dev/simple-deploy) it's a real
gotcha: a Secure cookie breaks the session over plain http. The docs, when they call `--mode http` a
"complete web server", should note this limit, OR consider making Secure conditional on localhost/http
(Secure behind TLS, not on plain http — but that's a security decision and should be measured).

### FINDING #9 — systemd template is Ubuntu-only (blows up on AlmaLinux) + HTTP-mode wrong binary

**Layer:** distribution / systemd / documentation platform assumption
**What I did:** I followed the systemd template in ubuntu-deployment.md §5 (the guide is titled
"Ubuntu/AlmaLinux").
**What happened (multiple):**
- `User=www-data` / `Group=www-data` → on AlmaLinux **there is NO www-data** (`no such user`), the web
  user is `apache` (uid 48). An AlmaLinux user copying the template verbatim can't start the service.
- **The HTTP-mode template says `ExecStart=/usr/local/bin/lk --mode http`** — but the CLI binary is `lk`,
  while `--mode http` is an `lk-fcgi` feature. `lk --mode http` → HTTP 000 (won't connect). Correct:
  `lk-fcgi --mode http`. A user following the HTTP-mode template gets a NON-WORKING service.
- **Permissions undocumented:** running the service as `User=apache` means it can't write to the
  session/upload/DB directories → `chown -R apache:apache /opt/dogfood` was required; the docs don't say so.
**WORKED with adaptation:** apache user + lk-fcgi + chown → systemd service active, enabled at boot, :7700→302,
end to end (register/task/JSON) with an apache-owned tasks.db, 0 fallback.
**Fix:** the systemd template for AlmaLinux needs `User=apache`, HTTP-mode `lk-fcgi`, and a chown/permission step.

### FINDING #10 — Secure cookie is unconditional + X-Forwarded-Proto not read (production analysis of #8)

**Layer:** session / reverse-proxy / TLS
**Source analysis:** the `Secure` flag is **hardcoded unconditionally** in web_stdlib.cpp (788/805/840);
the code reads **X-Forwarded-Proto / scheme** nowhere (grep clean). There is NO env to disable Secure.
**Conclusion:** in production (HTTPS reverse-proxy: browser↔proxy TLS) it works CORRECTLY — the cookie is
taken over HTTPS. localhost plain-http also works (browsers/modern-curl treat localhost as secure). The one
BROKEN scenario: non-localhost plain-http (a real deployment without TLS) — already an insecure config. So
#10 is LOW severity, but the docs, when they call `--mode http` a "complete web server", should note
"session requires HTTPS or localhost". Since X-Forwarded-Proto is not read, LOOK doesn't know its own
scheme (not needed for the cookie since Secure is unconditional; the scheme isn't used elsewhere either —
redirects are relative, UPLOAD_URL is manual).

### FINDING #11/#12 — SELinux is nowhere in the docs · app logs go to journalctl (undocumented)

**#11 (SELinux):** this VPS is `Permissive` (Plesk-configured) → I could NOT test the enforcing wall. But a
fresh AlmaLinux defaults to **Enforcing** → the upload directory/DB/port-bind may need SELinux
context/booleans. The docs mention SELinux NOWHERE → a likely wall for a fresh-install user (couldn't be
measured, flagged).
**#12 (logging/diagnostics):** the INFO/ERROR logs of a systemd `--mode http` app go to **journald** →
`journalctl -u <service>`. It works, but docs §5 only shows `systemctl status`; the error-diagnosis path
(journalctl -u, log lines) isn't documented clearly. The deployment form of the second analyzer's
"diagnostics layer is weak" observation.

### DEPLOYMENT ROUND RESULT (build-from-source + run were dogfooded)

The task manager was built from source on the VPS (AlmaLinux 8.10) and RUN on :7700, with the full
CRUD+auth+JSON flow verified (including session::has — absent from the released binary); the live
`look-test-codlook-com` :9100 + test.codlook.com→200 were NOT TOUCHED (PID-kill cleanup). **3 undocumented
assumptions surfaced** (#6 gcc-toolset, #7 static-SSL default, #8 Secure-cookie/http) — as the analyzer
predicted, "deploy always carries undocumented assumptions; writing code is doc-guided, deploying is not".
REMAINING (not done this round): systemd unit persistence, Apache HTTPS wiring, logging/permission/DB-path
documentation.

### FINDING #1 — `session::has()` is documented, but the implementation is MISSING (500)

**Layer:** session / doc-implementation mismatch
**What I did:** For the CSRF token on the register page I wrote `if (!session::has("csrf"))` — the pattern
from the docs. The auth example (docs line 1584) also uses `session::has("admin_id")`.
**What happened:** GET /register → **HTTP 500**, empty body. Log:
- VM: `Not callable (function expected)` → the route fell back to the interpreter (VM BUG log)
- Interpreter: `'session' has no function 'has'`
**What the docs said:** `session::has("k")` — "Does the key exist?" (docs line 1549) + it's used in the
docs' own auth example (1584). So the docs PROMISE the API, the core doesn't provide it.
**Root:** the session module in `web_stdlib.cpp` has `start/regenerate/set/get/destroy`, no `has`; it's also
absent from `builtin_names()` → "not callable" in the VM, "no function" in the interpreter.
**Why it matters:** EVERYONE following the docs hits this; the auth example itself doesn't work. Ten rounds
of audit didn't see it because the tests didn't call `session::has` — "a surface the guards don't cover".
**Fix:** add `has` to the session module (like get but bool), add `session::has` to the END of
`builtin_names()` (not the middle — .lkc index shift). + the docs were already correct, the code caught up.
**Side note (separate, small):** the VM fallback error "Not callable" doesn't say which name couldn't be
called. The interpreter message ("'session' has no function 'has'") is far more useful. The VM side should
give the name too (diagnosis convenience).

### FINDING #2 — Ceil-division papercut: a float leaks into the pagination UI (DX, my own code)

**Layer:** arithmetic / DX (NOT a language bug — documented behavior, but the friction is real)
**What I did:** `$total_pages = ($total + $PER_PAGE - 1) / $PER_PAGE` — the classic ceil-division idiom.
**What happened:** the template showed "Page 1 / **2.2**". Because `11/5 = 2.2` (int/int → float, documented).
**Root:** LOOK has no integer-division operator (`//` / `div`). The most common web pattern (pagination)
needs an `int(...)` wrapper every time; it's easy to forget and silently leaks a float into the UI.
**Workaround/fix:** `int((...)/...)` — `int()` floors a positive float, and ceil-division checks out.
**Suggestion (language, DEFER):** the philosophy is "the shortest path" — since pagination is this common,
a `//` integer-division operator could be considered. BUT float division is a deliberate/documented
decision; adding it now is out of scope. If one more user hits this, the weight grows. Recorded as a
papercut for now.

### FINDING #3 — File upload: no multipart in `--mode http` + two sub-bugs

**Layer:** file:: / request::file() / http server / PHILOSOPHY tension
**What I did:** I wanted to add a file attachment to a task (a core CRUD pattern). I checked the docs:
`request::file("doc", [...])` + `file::store()`. The docs warning (line 2432): multipart is NOT parsed in
`--mode http`, `request::file()` returns null, use base64-in-JSON or FastCGI.
**What happened (empirical, WORSE than the docs):**
- **A (doc↔impl):** the docs say "returns null"; in reality it **throws an exception**:
  `request::file() requires multipart/form-data request`. The `if ($f == null)` branch is NEVER reached →
  the error propagates. A sibling of the session::has class (the docs promise a contract, the code does
  something else).
- **B (robustness):** a multipart POST → **HTTP 000 (connection reset)**, not a clean 500. A base64-JSON
  body to the same route → clean HTTP 500. The difference: while the multipart body is being processed the
  connection drops (the request body is not drained before the response/close → TCP RST). curl never gets
  a response.
**PHILOSOPHY tension (the real issue):** LOOK claims "single-exe web (`--mode http`) + CRUD without a
framework"; but file upload is a core CRUD pattern, and in EXACTLY that mode a standard HTML `<form
enctype=multipart/form-data>` doesn't work. The user has to choose either JS+base64 (not a plain HTML
form) or FastCGI (which breaks single-exe simplicity). That's a gap in the "deploy easily like PHP" story.
**Decision point (like ASSOC — not by guessing, only after measuring):** three paths —
  (1) parse multipart in `--mode http` (philosophy-consistent, a core http_server feature — big),
  (2) accept the limit honestly: request::file() should RETURN null (match the docs, close A) + clarify docs,
  (3) fix B (connection reset) regardless: a throwing route must drain the body and return a clean 500.
  B is a robustness bug INDEPENDENT of the upload decision (any error → RST is unacceptable).

**LATEST UPDATE — B WITHDRAWN (test artifact), THE REAL FINDING: DOCS STALE.**
The measure-first discipline (the analyzer's insistence) disproved all three hypotheses:
- **B DOESN'T EXIST — TEST ARTIFACT.** The presumed "5/5 crash" from `curl -F "@f;type=text/plain"` gives
  **exit 26** in this Windows/git-bash curl (curl can't read the file — the `;type=` suffix breaks
  path-parsing), the request NEVER reaches the server → HTTP 000. NOT a server crash. The twin of the
  UTF-8 mojibake. Correct measurements: hand-crafted `/dev/tcp` (200), `--data-binary @multipart_body`
  (200), ASan SILENT (no memory error), the server withstood 20+ requests (no hang/DoS), the parser
  handles a well-formed multipart correctly.
- **Multipart ALREADY WORKS in `--mode http`.** `request::file()` DID return the file on a real multipart
  POST (`{"got":"file","size":11}`, 0 fallback). The code comment (http_main.cpp:708-716) confirms it too:
  parsing was added, "the README wrote it as a 'known limit' but it was a forgotten branch".
- **THE REAL FINDING (doc↔impl, reversed direction): DOCS STALE.** The upload section (docs/index.html:2432)
  still says "`--mode http` does NOT parse multipart, request::file() returns null, use base64/FastCGI" —
  but it DOES parse. The user is needlessly steered to a base64 workaround or FastCGI, or avoids single-exe
  upload thinking it doesn't work. docs/ is gitignored (website) → the user must fix it.
- **A RESOLVED (durable, `8fb8267`):** request::file() returns null on non-multipart (the docs contract).
  It doesn't affect the working multipart path (that content-type is multipart → returns the file, proven).

**META LESSON:** the analyzer's insistence on "measure first, don't write speculative security code" saved
me from (1) a fix for a nonexistent bug, (2) a whole round for an already-finished feature, (3) a fake DoS
escalation. Measurement disproved the hypothesis a 4th time this session (mojibake, count-visibility,
ceil-papercut, now B).

### FINDING #4 — Upload is USABLE, but magic-byte MIME rejects plain text

**Layer:** file:: / request::file / MIME validation / DX
**What I did:** I wrote a file-attach screen for a task (attach.html multipart form + request::file +
file::store + allow_mime + size limit + error display). I measured "is it USABLE", not "does it parse" (as
the analyzer said).
**Happy path WORKS (PNG, magic-byte):** upload→302 flash, storage/task-1/<sha256>.png written, DB url
recorded, 📎 in the list, 0 fallback. The full path (request::file + file::store + web-root protection +
DB) works fully in `--mode http` → the docs:2432 warning is DEFINITELY stale (the full path verified).
**What happened (FINDING):** uploading a .txt with `allow_mime:["text/plain"]` → **"File type not allowed:
application/octet-stream"**. The magic-byte detector sees plain text (which HAS no magic byte) as
octet-stream → text/plain never matches. Because the docs examples always use IMAGES (jpeg/png have magic
bytes) this blind spot is masked. **Conclusion:** it is IMPOSSIBLE to allow magic-byte-less formats like
text/csv/log with allow_mime (the only way is to add "application/octet-stream" = allow every binary =
defeats the security purpose).
**Suggestion (language, CONSIDER):** (a) a heuristic "if no magic byte matches and all bytes are printable
→ text/plain", OR (b) an option to trust the declared Content-Type for text-based types, OR (c) a docs note
"allow_mime only works for formats with magic bytes". At the very least the docs example misleads by using
only images.
**Test-harness note:** curl `-F "@f;type=..."` gives exit 26 in this environment → upload tests were done
with hand-crafted multipart + `--data-binary` (reliable). Also, forgetting to delete data.db caused a
silent "email already registered" register-fail → 302/login (again my own state error, not LOOK).

--- (below is a record of the withdrawn B analysis — historical) ---
**B confirmed (separate round):**
- **A RESOLVED:** `web_stdlib.cpp` request::file() now returns null instead of throwing on a non-multipart
  request (the docs contract). Verified: JSON/urlencoded POST → `file=null`, clean HTTP 200.
- **B DEFINITE REPRO (a separate focused round — the security surface the analyzer mentioned):** not the
  "throw→RST" I first suspected. Deterministic: when the multipart file part includes **its own
  `Content-Type` header** (`curl -F "doc=@f;type=text/plain"`) → **5/5 HTTP 000** (worker hang/reset, no
  log, process alive). Without the header (`-F doc=@f`) → 5/5 HTTP 200. **CRITICAL: real BROWSERS ALWAYS
  send a Content-Type on the file part** → this isn't a curl edge case, the NORMAL browser upload path
  brings the worker down (`--mode http`). A likely worker-exhaustion DoS (N concurrent uploads → N hung
  workers). The parse_multipart header loop appears to ignore the Content-Type line but the behavior says
  otherwise — the parser is body-offset or Windows-specific. Diagnose in the multipart ROUND: (a) hang or
  crash, (b) measure worker-exhaustion DoS, (c) does it reproduce on Linux (once docker is available).

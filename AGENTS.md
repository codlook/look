# AGENTS.md — Working on the LOOK repositories

Context for AI agents that read, modify, or open pull requests against the Codlook / LOOK
repositories. Read this first, then the repo-specific files it points to.

## What LOOK is

LOOK is a **scripting language built for the web** — one static binary with **minimal external
runtime dependencies**. It embeds MySQL/PostgreSQL/SQLite (wire protocols implemented in-house),
WebSocket, Server-Sent Events, and an SMTP/IMAP mail server. A complete web app runs from one
`.lk` file: you write routes, database queries, templates and background jobs; the runtime handles
concurrency, connection pooling and hot reload. Default engine is a register-based **bytecode VM**,
with a tree-walk interpreter as a fallback.

LOOK is **its own language for the web** — it is *not* positioned as a replacement for PHP, Node,
Go or Rust. Comparisons are only for measurement/context, never as an "X vs LOOK" or "use LOOK
instead of X" frame.

## The ecosystem (repositories & sites)

| Repo | Purpose |
|------|---------|
| `github.com/codlook/look` | **Main repo** — the language runtime (C++23), tools, docs and website. |
| `github.com/codlook/look-modules` | Global modules installed with `lk module install` (e.g. `jwt`, `ai`). |
| `github.com/codlook/look-packages` | Per-project packages installed with `lk install` (e.g. `firebase`, `iyzico`, `monitor`). |

Websites (served from `docs/` in the main repo):
- **look.codlook.com** — the LOOK language site (source: `docs/look.codlook.com/`), English.
- **codlook.com** — the company site (source: `docs/codlook.com/`), Turkish.
- **looky.codlook.com** — Looky, the AI assistant for LOOK (source: `docs/looky.codlook.com/`).
- AI/SEO files live at each site root: `llms.txt`, `llms-full.txt`, `sitemap.xml`, `robots.txt`, plus per-page JSON-LD.

## Main repo layout

```
cpp/                 C++23 runtime — the language itself
  src/               lexer.cpp, parser.cpp, compiler.cpp, vm/interpreter, stdlib (*_stdlib.cpp),
                     db clients (mysql/postgres/resp), servers (http/smtp/imap), fibers, jobs…
  include/look/      headers (ast.h, bytecode.h, builtins.h, crypto_sha256.h, wire parsers…)
  tests/             differential + guard scripts, fuzzers
  CMakeLists.txt · Dockerfile.build
docs/                websites (look.codlook.com, codlook.com, looky.codlook.com) + *.md guides
platforms/           linux (installers, rpm), plesk (extension), vscode (extension), windows
dogfood/             apps written in LOOK, used to dogfood the language
.github/workflows/   ci.yml, sanitizers.yml, security.yml, windows.yml, release*.yml, docker-publish.yml
README.md · SECURITY.md · CLAUDE.md · LICENSE
```

Modules/packages are LOOK source: a module is `<name>/<name>.lk` + `README.md`; a package is
`<name>/<name>.lk` (+ optional `docker/`, `test_*.lk`).

## Golden rules (non-negotiable)

1. **Git identity — Codlook only.** Author/committer is `Codlook`. **Never** add a `Co-Authored-By`
   line (no Claude, no other names). A commit-msg hook rejects violations.
2. **Commit messages in English**, prefixed `Fix:` / `Feat:` / `Perf:` / `Docs:`. Commit or push
   only when asked; if on the default branch, branch first unless told otherwise.
3. **Language policy.** LOOK is international. **Everything public-facing is English** — commit
   messages, PR/issue text, release notes, docs, landing pages, and every user-facing runtime
   string (thrown errors, logger output, `--check`/`--version`/`--help`, installer messages).
   Turkish stays only for internal working notes and **code comments** (internal, not user-facing).
4. **Positioning wording.** Never write "zero dependency" — the runtime links OpenSSL/glibc. Use
   **"minimal external runtime dependencies"**, or "self-contained" / "no external driver" for a
   specific in-house feature (wire protocols, codecs, package manager).
5. **Never commit secrets.** No passwords, tokens, private keys, or server IPs. Deployment
   credentials are **not** in this repo and must never be added — ask the maintainer for them.
   CI secrets live in GitHub Actions settings, referenced by name only.

## Build & run

Linux binary is built in Docker (Windows/host builds go through the image):

```bash
# one-time: bake the toolchain image
docker build -t look-build -f cpp/Dockerfile.build cpp/
# build a target (use -j8, not $(nproc); rm -rf build on the first Windows-mounted build)
docker run --rm -v "$PWD/cpp:/look/cpp" -w /look/cpp look-build bash -c "cmake --build build --target look-fcgi -j8"
```

Binaries: `lk` (CLI/REPL/test), `look-fcgi` (web: FastCGI + `--mode http` for WebSocket),
`lk-cgi` (fallback). There are **no** feature flags like `LOOK_ENABLE_MYSQL` — DB engines are
always embedded. Runtime tuning is via `LOOK_*` environment variables (documented at
look.codlook.com/docs.html → Environment Variables).

## Engine & test guards

- **Bytecode VM is the default** for `lk` and `look-fcgi`; tree-walk is the fallback. Undefined
  variables are a **hard error** in both engines (`LOOK_WARN_UNDEF=1` restores lenient mode).
- After **any** engine change, run the guards:
  - `bash cpp/tests/differential_test.sh <lk> <lk-fcgi>` — 3 engines × many categories must agree.
  - `bash cpp/tests/parallel_db_test.sh <lk-fcgi>` — parallel + DB isolation (one request isn't enough).
- CI (`.github/workflows/`) runs the build, sanitizers (ASan/UBSan/TSan), security/fuzz, Windows,
  and release consistency. Keep it green; a "green" run with 0 jobs means the workflow failed to
  start — verify jobs actually ran.

## Website generators (don't hand-edit generated output blindly)

- `docs/look.codlook.com/errors.html` is **generated** from `docs/errors.md` (plus a curated
  wrong→right code-example map). Edit the source/generator, then regenerate.
- `docs/look.codlook.com/packages.html` was generated from package data that no longer lives in the
  repo — it is now **hand-edited directly**.
- When you change docs, keep the machine-readable files in sync: `llms.txt`, `llms-full.txt`
  (the whole docs as one Markdown), `sitemap.xml`, and each page's JSON-LD.

## Where to look next

- `README.md` — project overview and resource links.
- `SECURITY.md` — security model; the protocol parsers are our own code, hardened and fuzzed.
- `CLAUDE.md` — the maintainer's detailed working instructions (Turkish; build/deploy/engine notes).
- `docs/*.md` — deployment and protocol guides (ubuntu, plesk, smtp, imap, package-system).
- look.codlook.com/docs.html — the full language reference; /errors.html — every error with a fix.

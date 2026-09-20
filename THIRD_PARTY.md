# Third-party components

LOOK has **minimal external runtime dependencies** — there is nothing to `npm install`,
`pip install`, or `composer require`, and no language runtime to ship alongside it. The
database drivers (SQLite, MySQL/MariaDB, PostgreSQL, Redis/RESP2), the template engine,
the HTTP/SMTP/IMAP servers, JSON, and the crypto plumbing are all our own code, compiled
into the binary.

A few well-known, self-contained components are **vendored** (their source lives in the
tree and is compiled in) or **statically linked** at release time. They are listed here in
full — "minimal dependencies" means *few and pinned*, not *none hidden*.

| Component | Version | License | How it's used | In the tree |
|---|---|---|---|---|
| **SQLite** | 3.53.4 | Public domain | Embedded SQL database driver (the amalgamation) | `cpp/src/sqlite3/sqlite-amalgamation-3530400/` |
| **miniz** | 11.0.2 | MIT | Deflate / zlib + ZIP (gzip responses, archive support) | `cpp/src/miniz/` |
| **linenoise** | pinned upstream snapshot | BSD-2-Clause | Line editing for the `look repl` interactive shell | `cpp/src/linenoise/` |
| **OpenSSL** | 3.5.8 | Apache-2.0 | TLS for `http::`, DB drivers, SMTP/IMAP; crypto primitives | Statically linked in release binaries (see below) |

## How each is shipped

- **SQLite, miniz, linenoise** are vendored as source and compiled directly into
  `lk` / `lk-fcgi` / `lk-cgi`. Pinning the source (rather than resolving a package at build
  time) is deliberate: the build is reproducible and the audited surface never shifts under
  us. Upgrades are an explicit commit that bumps the vendored copy.

- **OpenSSL** is the one component linked rather than vendored as source:
  - **Release binaries (portable / Docker / Plesk)** statically link a pinned OpenSSL
    **3.5.8** (a 3.5.x LTS release, supported upstream until 2030), built with a verified
    SHA-256 by `cpp/build-portable.sh` / `cpp/Dockerfile.build`, so the artifact has no
    external `libssl` / `libcrypto` requirement.
  - **The RPM package** links the OS-managed `openssl-libs` instead, so
    `dnf update openssl-libs` applies OS security patches without rebuilding LOOK — the
    normal distro trade-off (patch cadence over self-containment).

## Updating a vendored component

1. Replace the source under `cpp/src/<component>/` with the new upstream release.
2. Update the version (and, for SQLite, the amalgamation directory name) in this file.
3. Rebuild and run the full guard suite (`differential_test.sh`, `parallel_db_test.sh`,
   the fuzz/sanitizer jobs) before committing — a vendored bump is a real code change.

## Toolchain (build-time only, not shipped)

CMake (≥ 3.20) and a C++17 compiler (GCC 12 / Clang / MSVC) build LOOK; they are not part
of the runtime and are not distributed with it.

# LOOK Language — Plesk Extension

One-click LOOK web-runtime hosting for Plesk. Publish any domain as a LOOK
application (its own systemd service) with a built-in code editor, live
per-domain monitor, log viewer and system dashboard.

- Minimal external runtime dependencies — bundles the portable `lk` binary + RPM
- Full workspace UI: Dashboard · Applications · Logs · Documentation
- In-browser code editor (edit `index.lk` → save → auto redeploy)
- Live monitor (CPU, RSS, PID, connections, uptime, restarts) + log tail

Requires Plesk Obsidian 18.0+ on a systemd Linux host (AlmaLinux/RHEL/Ubuntu).

---

## Supported platforms

The bundled binary is a **single portable static build** (OpenSSL + libstdc++ linked
in; only glibc + libz dynamic). It requires **glibc ≥ 2.28** and runs on every
currently-supported Linux distribution:

| Distribution | Status |
|---|---|
| AlmaLinux / Rocky / RHEL / CloudLinux **8, 9, 10+** | ✅ Supported |
| Ubuntu **18.10 → 24.04+** | ✅ Supported |
| Debian **10 (Buster) and newer** | ✅ Supported |
| **CentOS 7 / RHEL 7** (glibc 2.17, EOL 2024-06) | ❌ Not supported — migrate to **AlmaLinux 8** (free drop-in) |

**Forward-compatible by design.** Because the binary is built on an *old* glibc (2.28)
and statically bundles OpenSSL, it keeps working on **future** distributions automatically:

- **New glibc** (2.38, 2.40, …): glibc is forward-compatible — an old-glibc binary runs on
  newer glibc. No rebuild needed for AlmaLinux 10, Ubuntu 26, etc.
- **New OpenSSL** (3.x, 4.x): the binary carries its own OpenSSL, so the system's version is
  irrelevant.

So new distro releases need **no new binary**. Two long-term maintenance notes (not per-distro):
1. Keep building on the **oldest supported glibc** (AlmaLinux 8) — never raise the floor.
2. Periodically refresh the bundled OpenSSL (1.1.1 → 3.x) for security hygiene.

---

## Quick install (from the Plesk terminal)

Run as **root** on the server (Plesk → Tools & Settings → Terminal, or SSH):

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk.zip "https://github.com/codlook/look/releases/latest/download/look-lang-plesk.zip"
plesk bin extension --install /tmp/look-lang-plesk.zip
```

Or via Plesk UI: **Extensions → Upload Extension → `look-lang-plesk.zip`**.

Open the panel: **Plesk → Extensions → LOOK Language**, or directly at
`https://<server>:8443/modules/look-lang/`.

### What installing and updating do

Every install and every update of the extension, from the UI or the CLI, runs its
setup as root:

- installs the bundled LOOK binaries to `/opt/look`, replacing the old ones;
- writes the sudoers rule `/etc/sudoers.d/look-lang` the panel needs;
- rewrites the service of every domain in the panel with the current rules and
  restarts it.

A domain whose service cannot run as the domain's user (for example a document
root owned by root) is **stopped**, not left running as root. The install then
reports an error that names the domain. Fix the cause and install the extension
again, or run the setup again as root:

```bash
plesk bin extension --exec look-lang post-install.php
```

---

## How a domain runs

Each domain added in the panel gets its own systemd service, `look-<domain>.service`:

- **User** — the service runs as the domain's system user (the owner of its document
  root), never as root. A document root owned by root or a system account is refused.
  Ports below 1024 need root and are refused too.
- **Script** — the `.lk` file chosen in the panel (default `index.lk` in the document
  root). It must be inside the domain's document root. When it does not exist, a small
  sample app is created, owned by the domain's user.
- **Env file** (optional) — a file of `KEY=VALUE` lines inside the document root, for
  example `.look.env`. The domain's user reads it when the service starts, so it can be
  mode `600`. Lines starting with `#` are comments; there are no comments at the end of
  a line: in `APP_NAME=Shop # title` the value is `Shop # title` (the service log warns
  about it). Quotes around a value are removed.
- **Remove** stops the service and deletes `look-<domain>.service` and its
  `look-<domain>.service.d` folder; **Stop** only stops it.

## Update

Install a newer zip the same way — Plesk replaces the existing extension in
place (domain configuration in `/usr/local/psa/var/modules/look-lang/` is
preserved).

Updating from 1.1.x or 1.2.0: the update rewrites every domain's service to run as the
domain's user (1.2.0 itself did not: its update left the old binary and the old services
in place). Files the old root service created (an SQLite database, uploads) may be
owned by root; the setup output says how many per domain, and
`chown <user>:psacln <file>` gives them back to the domain's user.

## Uninstall

```bash
plesk bin extension --uninstall look-lang
```

Per-domain LOOK services (`look-<domain>.service`) and their `index.lk` files
are left untouched; remove them from the panel first if you want a full cleanup.

---

## Building the zip

The published zip is produced from this directory. On Linux:

```bash
bash platforms/plesk/build.sh --with-binaries
```

It bundles `meta.xml`, `post-install`, `plib/`, `htdocs/` (UI + scripts +
portable binaries) and the RPM into `look-lang-plesk.zip`.
The zip must use forward-slash paths with `meta.xml` at the archive root.

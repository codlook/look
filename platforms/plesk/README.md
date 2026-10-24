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

### Post-install setup — one command (REQUIRED on new servers)

Plesk does **not** reliably run the extension's `post-install` hook (confirmed: the
`plesk bin extension --install` CLI / sideload install never runs it). So a fresh
install does **not** auto-configure the engine or sudoers. **Immediately after
installing**, run this **once as root** — it installs the portable binary to
`/opt/look` **and** writes the sudoers rule (survives uninstall/install cycles):

```bash
plesk php /usr/local/psa/admin/plib/modules/look-lang/scripts/post-install.php
```

Without this step, enabling a domain fails with:

```
sudo: a terminal is required to read the password ...
sudo: a password is required
```

Reason: with no sudoers rule the panel's `sudo systemctl …` call prompts for a
password, but the web context has no terminal. The command above creates
`/opt/look/lk-fcgi` + `/etc/sudoers.d/look-lang`, after which enabling a domain
works. (Equivalently you may run the same logic directly:
`bash /usr/local/psa/admin/htdocs/modules/look-lang/scripts/setup.sh`.)

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

Updating from 1.1.x: services created by 1.1.x run as root until they are written
again. Open the panel and press **Restart** on each domain — the service is rewritten
to run as the domain's user. Files the old service created (an SQLite database,
uploads) may be owned by root; the restart output says how many, and
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

# LOOK Plesk Extension — Installation Guide

> Verified on AlmaLinux 8.10 + Plesk Obsidian.
> Full reference: [plesk-extension.md](plesk-extension.md).

## Requirements

- Plesk Obsidian 18.0+
- systemd Linux (AlmaLinux/RHEL/Rocky/CloudLinux · Ubuntu/Debian Plesk)
- Root access (SSH or Plesk → Tools & Settings → Terminal)

---

## Supported systems

The bundled binary is **a single portable static build** (OpenSSL + libstdc++ built in; only
glibc + libz are dynamic). It needs **glibc ≥ 2.28** and runs on every supported distribution:

| Distribution | Status |
|---|---|
| AlmaLinux / Rocky / RHEL / CloudLinux **8, 9, 10+** | ✅ Supported |
| Ubuntu **18.10 → 24.04+** | ✅ Supported |
| Debian **10 (Buster) and later** | ✅ Supported |
| **CentOS 7 / RHEL 7** (glibc 2.17, EOL 2024-06) | ❌ Not supported — move to **AlmaLinux 8** (free, a one-to-one successor) |

**Forward compatible by design.** Because the binary is built against an *old* glibc (2.28) and
carries OpenSSL statically, it runs on **future** distributions as is:
- **Newer glibc** (2.38, 2.40…): glibc is forward compatible → a binary built on an old glibc runs
  on a newer one. AlmaLinux 10 and Ubuntu 26 need no new build.
- **Newer OpenSSL** (3.x, 4.x): the binary carries its own OpenSSL → the system version does not
  matter.

So new distribution releases **do not need a new binary.** Releases keep building on the oldest
supported glibc (AlmaLinux 8), and the bundled OpenSSL is updated for security fixes.

---

## Step 1 — Install

On the server, as root:

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk.zip "https://github.com/codlook/look/releases/latest/download/look-lang-plesk.zip"
plesk bin extension --install /tmp/look-lang-plesk.zip
```

Expected output: `The extension was successfully installed.`

Alternative — Plesk UI: **Extensions → Upload Extension → `look-lang-plesk.zip`**.

### What install and update do

Every install and update (UI or CLI) runs the extension's setup step as root:

- installs the LOOK binaries from the package into their own release directory under
  `/opt/look/releases/` and points `/opt/look/lk*` at the newest one (a release a site still runs
  is kept);
- deletes the sudoers rule of older versions (`/etc/sudoers.d/look-lang`) — the panel does not use
  sudo, it does its root operations with the extension's `look-ctl` script;
- rewrites the service of every domain in the panel to the current rules and restarts it (the
  service runs as the domain's user). Before that, the newest release's `lk --check` is run on the
  site's code: a site whose code does not pass moves to the newest release it does pass, or stays
  on the release it ran (see
  [Binaries and LOOK releases](plesk-extension.md#binaries-and-look-releases)).

A service that cannot run as the domain's user is **stopped**, and the installation fails with an
error that names the domain. A site whose code does not pass and that has no earlier release to
stay on is left as it was and named in the error ("not moved to the new LOOK"). Fix the cause, then install the extension again or run the step again as root:

```bash
plesk bin extension --exec look-lang post-install.php
```

---

## Step 2 — Open the Panel

**Plesk → Extensions → LOOK Language**, or `https://<server>:8443/modules/look-lang/`.

Workspace left sidebar: **Dashboard · Applications · Logs · Documentation**.

---

## Step 3 — Add a Domain

1. **Applications** tab → **Add New Domain** form.
2. Pick a domain (the Plesk domains are listed) — the script path and a free port are filled in.
3. Optional: an **Env file** (inside the domain user's home or the document root) and a
   **Memory limit** (`auto` by default: a quarter of the server's memory, at most 1G, at least
   256M; or `512M`, `2G`, `off`).
4. Pick a mode: **FastCGI** (recommended for production) or **HTTP**.
5. **Add & Start** → within a few seconds it shows as **Running** in the list.

---

## Step 4 — Write the Code

In the application table, **Edit Code** → edit `index.lk` → **Save & Redeploy**
(the file is written + the service is redeployed):

```lk
route("GET", "/", fn() => response::json(["ok" => true, "app" => "LOOK"]))
route("GET", "/ping", fn() => response::json(["status" => "ok"]))
route("404", fn() => response::error(404, "Not found"))
```

---

## Panel Features

| Feature | Description |
|---|---|
| **Dashboard** | Live CPU/Memory/Disk/Uptime + mini charts |
| **Add New Domain** | Creates the systemd service + Apache proxy config |
| **Edit Code** | In-browser `index.lk` editor → Save & Redeploy |
| **Monitor** | Live per domain: CPU/RSS/PID/connections/uptime + log tail |
| **View Logs** | `journalctl` viewer (colored + copy) |
| **Configure / Restart / Stop / Remove** | Service management (Actions menu) |

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Installation says "setup failed" | Fix the cause named in the error text, then install the extension again or, as root, run `plesk bin extension --exec look-lang post-install.php` |
| "not moved to the new LOOK" | Fix the lines listed (from `lk --check`), then use **Check again** in the panel |
| Domain added but not in the list | `ls -l /usr/local/psa/var/modules/look-lang/domains.json` (must be owned by psaadm) |
| Site 502/403 | `systemctl status look-<domain>` + `journalctl -u look-<domain> -n 50` |

Details: [plesk-extension.md](plesk-extension.md).

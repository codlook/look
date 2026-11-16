# LOOK Language — Plesk Extension

**Version:** 1.6.0
**Extension ID:** `look-lang`
**Target:** Plesk Obsidian 18.0+ · systemd Linux (AlmaLinux/RHEL/Rocky/CloudLinux · Ubuntu/Debian Plesk)
**Source:** `platforms/plesk/`
**Live example:** [test.codlook.com](https://test.codlook.com) — AlmaLinux 8.10, Plesk Obsidian, nginx→Apache→lk-fcgi

The LOOK Plesk extension publishes any Plesk domain as a **LOOK application** (its own
systemd service) in one click. With an in-browser code editor, a live per-domain monitor, a log
viewer and a system dashboard, it is a complete hosting workspace. The panel interface is in
**English**.

---

## Installation

On the server, as **root** (SSH or Plesk → Tools & Settings → Terminal):

```bash
plesk bin extension --uninstall look-lang 2>/dev/null
wget -O /tmp/look-lang-plesk.zip "https://github.com/codlook/look/releases/latest/download/look-lang-plesk.zip"
plesk bin extension --install /tmp/look-lang-plesk.zip
```

Expected output: `The extension was successfully installed.`

Alternative — Plesk UI: **Extensions → Upload Extension → `look-lang-plesk.zip`**.

Open the panel: **Plesk → Extensions → LOOK Language**, or directly at
`https://<server>:8443/modules/look-lang/`.

> **Installation note:** Plesk runs the extension's `post-install.php` hook on install and on
> upgrade, but as `psaadm`, not as root. The step that needs root (binaries, service units) lives in
> `sbin/look-setup`, and the hook calls it through Plesk's root path `pm_ApiCli::callSbin`; the
> panel's root operations take the same path to `sbin/look-ctl` (no sudo, see
> [Root operations](#root-operations-no-sudo) below). To run the step again by hand:
> `plesk bin extension --exec look-lang post-install.php`.

---

## Workspace (Panel)

The left sidebar has four views:

### Dashboard
Real-time system metrics — **CPU · Memory · Disk · Uptime**. CPU, Memory and Disk each have a
live mini chart (sampled every 4 seconds; there is no made-up history). Below them is an
application summary table (status, port, last deploy, configuration).

### Applications (Application Manager)
- **Add New Domain** form (above the list): pick a domain → the script path and a free port
  (the first free one from 9100) are filled in → optional env file, workers, memory limit and
  mode → **Add & Start**.
- **Env file** (optional): a `KEY=VALUE` file read by the service's own user at start. It must be
  inside the home directory of the domain's system user (recommended: outside the web root) or
  inside the domain's document root.
- **Memory limit**: the most memory the site may use (systemd `MemoryMax`). `auto` (default) =
  a quarter of the server's memory, at most 1G, at least 256M; `512M`, `2G` or `off`. A limit
  below 128M is refused. LOOK makes 80% of it its own limit: a request that would go over is
  stopped, the site keeps running.
- Application table: domain, live status, port, script, configuration (workers · mode · memory
  limit) and the LOOK release the site runs.
- **Edit Code** — edit `index.lk` in the browser → **Save & Redeploy**.
- **Actions ▾** menu: Configure · Restart · Stop/Start · **Monitor** · View Logs · Remove.

### Logs
Service journal (`journalctl -u look-<domain>`, last 200 lines), colored by level
(ERR/WARN/OK), copied to the clipboard with the **Copy** button.

### Documentation
In-panel LOOK quick start, sample route code, and a guide to modes and services.

### Code Editor
Reads the contents of `index.lk` → edit → **Save & Redeploy** writes the file (at most 1 MB) and
redeploys the service. Path safety is strict: only the `.lk` path registered in `domains.json`,
under `/var/www/vhosts`; `..` is refused.

### Monitor (per domain, live)
Real metrics every 3 seconds: **status, CPU%, memory (RSS), PID, active connections, port,
restart count, uptime** plus a live log tail. Restart straight from the dialog; polling stops
when it is closed.

---

## Architecture

Each domain is a **systemd service** that runs its `index.lk` file:
`look-<domain>` (lowercased; dots and other special characters become `-`). Web traffic:

```
nginx :443  →  Apache :7081 (vhost_ssl.conf ProxyPass)  →  lk-fcgi :<port>
```

- **FastCGI mode** (`fcgi`) — through Apache `mod_proxy_fcgi`; recommended for production.
- **HTTP mode** (`http`) — `lk-fcgi --mode http --bind 127.0.0.1`; Apache proxies to the local
  port. Versions before 1.4.2 listened on all interfaces (the port was reachable from outside);
  the update rewrites existing services too.

`enable.sh` writes the service and the Apache proxy config; `httpdmng --reconfigure-domain`
applies it in the background (the panel does not block).

Example: `example.com` → service `look-example-com`, port `9100`.

### Service user

A LOOK service **never runs as root**. The unit's `User=` is the domain's system user (the owner
of the document root). If the document root is owned by root or by a system account (uid below
1000), the service is not installed. The env file is read by that user too (through
`/opt/look/look-env`), so a symlink to another site's file stops with "Permission denied".

---

## Binaries and LOOK releases

The extension carries the portable static LOOK binaries (`lk`, `lk-fcgi`, `lk-cgi`): OpenSSL and
libstdc++ are built in, only glibc and libz are linked dynamically (minimal external runtime
dependencies, a single file each; the system CA store is detected automatically).

Releases are installed side by side: `/opt/look/releases/<version>-<stamp>/{lk,lk-fcgi,lk-cgi}`.
`/opt/look/lk*` point at the newest one (command line and new sites). The package can also carry
the latest release of the maintenance line (for example `bin-1.0/`).

On every install, upgrade, restart, save or **Check again**, `enable.sh` runs the newest
release's `lk --check` on the site's code — as the site's user, in its directory, with its
environment. `--check` only loads and compiles; it does not run the program.

- The code passes → the site runs the newest release.
- It does not pass, but a newer release of the maintenance line does → the site moves to that
  release and gets that line's fixes; the panel lists the lines to fix to reach the newest.
- Nothing newer passes → the site stays on the release it ran and **receives no fixes, security
  fixes included**; the panel shows this with the lines to fix and a **Check again** button.
- A new site whose code does not pass is not installed; the error lists the lines.

A release that no service uses any more is removed on the next install or upgrade (the newest
and the bundled releases always stay).

---

## State

The domain list is kept in Plesk's panel-writable module var directory:

```
/usr/local/psa/var/modules/look-lang/domains.json
```

> `/opt/look/conf` belongs to root (the binary directory); the panel runs as `psaadm` and cannot
> write there. That is why the state lives in the module var directory.

```json
[
  {
    "domain": "example.com",
    "script": "/var/www/vhosts/example.com/httpdocs/index.lk",
    "env": "",
    "workers": 4,
    "mode": "fcgi",
    "port": 9100,
    "memory": "auto",
    "svc": "look-example-com"
  }
]
```

Older entries without `env` or `memory` mean no env file and an automatic memory limit. The
release status of each site is written by root to `/opt/look/conf/status/<svc>.json`; the panel
only reads it.

---

## Root operations (no sudo)

The panel runs as `psaadm` and **does not use sudo**. Every operation that needs root — setting up
or stopping a service, status, logs and the monitor — goes through Plesk's root path
`pm_ApiCli::callSbin` to the extension's `sbin/look-ctl` script. `look-ctl` checks the action,
the argument count and the service name (`^look-[a-z0-9-]+$`, one service per call); it cannot
touch any other service or file.

```
look-ctl enable  <domain> <script> <workers> <mode> <port> [<env-file> [<memory>]]
look-ctl disable <svc> [<domain>] [remove]
look-ctl start   <svc>
look-ctl status  <svc>
look-ctl logs    <svc> <lines>
look-ctl monitor <svc>
```

Versions up to 1.0.33 wrote `/etc/sudoers.d/look-lang`. Since `*` in sudoers also matches spaces,
the rule `systemctl enable look-*` also allowed `systemctl enable look-a /tmp/evil.service` and
`systemctl stop look-a sshd`. The update deletes that file.

---

## Package Layout

```
look-lang-plesk.zip
├── meta.xml                              # Plesk extension definition
├── plib/controllers/IndexController.php  # Plesk Obsidian MVC bridge
├── plib/scripts/post-install.php         # install/upgrade hook (runs as psaadm)
├── sbin/
│   ├── look-setup   # root install step: binaries, services (called with callSbin)
│   └── look-ctl     # the panel's only way to root (called with callSbin)
└── htdocs/
    ├── index.php                  # backend (AJAX actions + render)
    ├── phtml/index.phtml          # workspace interface
    ├── bin/{lk,lk-fcgi,lk-cgi}    # portable static binaries (newest release)
    ├── bin-1.0/                   # maintenance line release (when bundled)
    └── scripts/
        ├── enable.sh   # domain → systemd service + Apache proxy config
        ├── disable.sh  # stop/remove the service
        ├── status.sh   # service status
        ├── logs.sh     # journalctl (log viewer)
        ├── monitor.sh  # live metrics (Monitor)
        ├── look-env.sh # loads the env file as the service user
        └── setup.sh    # re-runs the install step by hand
```

> **Packaging note:** Plesk unpacks the zip with PHP `ZipArchive`, which does not keep the unix
> execute bit. So the zip's exec permissions do not matter (the scripts are called with
> `/bin/bash`); only **forward-slash paths + LF line endings + `meta.xml` at the root** are
> required. On Linux, `bash platforms/plesk/build.sh` produces the correct package.

---

## Service Management

```bash
enable.sh <domain> <script> <workers> <mode> <port> [<env-file> [<memory>]]
```

- `<mode>`: `fcgi` (default) or `http`.
- `<port>`: 1024–65535 (the service does not run as root, so it cannot open a lower port).
- `<script>`: must be inside the domain's document root, under `/var/www/vhosts`.
- `<env-file>`: optional; inside the domain user's home or the document root, readable by that
  user.
- `<memory>`: `auto` (or empty) = a quarter of the server's memory, at most 1G, at least 256M;
  `off` = no limit; otherwise megabytes or gigabytes such as `512M` or `2G`. Below 128M is refused.
  Written to the unit as `MemoryMax=`.
- systemd unit: `/etc/systemd/system/look-<domain>.service` (`User=` the domain's user,
  `NoNewPrivileges=true`, `PrivateTmp=true`, `Restart=on-failure`,
  `Environment=LOOK_FILE_ROOT=<document root>`)
- When the domain user's home is under `/var/www/vhosts` (the Plesk default): sessions are kept
  per site in `~/.look/sessions-<svc>`, and
  if the site has no `LOOK_SECRET`, a random one is created in `~/.look/<svc>.secret`
  (mode 0600, as the domain user, outside the web root); a `LOOK_SECRET` in the site's own env
  file wins.
- Apache proxy: `/var/www/vhosts/system/<domain>/conf/vhost.conf` + `vhost_ssl.conf` (the proxy
  rule, a rule that denies `/.look*` files, and `X-Forwarded-Proto` set by Apache)
- If the script does not exist, a sample `index.lk` is created (as the domain user)
- `httpdmng --reconfigure-domain` reloads Apache in the background
- Output ends with `OK:<svc>:<port>`; exit code 3 means the code does not pass `lk --check` (see
  [Binaries and LOOK releases](#binaries-and-look-releases)).

`disable.sh <svc> [<domain>] [remove]` stops and disables the service; `remove` also deletes the
unit file. The vhost files then keep only the rule that denies `/.look*` files, so `/.look.env`
cannot be downloaded while the site is stopped.

---

## Security Environment Variables

Set them in the site's env file (the **Env file** field in the add form or in Configure):

| Variable | Description |
|---|---|
| `LOOK_FILE_ROOT` | Limits file access to this directory; path traversal is blocked. The unit sets it to the domain's document root. `*` removes the limit (opt-out) |
| `LOOK_RATE_LIMIT_RPM` / `_BURST` | Per-IP limit per minute + burst (token bucket) |
| `LOOK_RATE_LIMIT_GLOBAL_RPM` / `_BURST` | Limit across all IPs — botnet protection |
| `LOOK_TRUSTED_PROXY` | `X-Forwarded-For` from this IP is accepted as the real IP |
| `LOOK_SESSION_DRIVER=redis` + `LOOK_REDIS_URL` | Multi-server sessions (RESP2 built into the core) |

---

## Troubleshooting

**Installation failed with "setup failed"** — the error text names the cause and the domain whose
service was stopped. Fix the cause, then install the extension again or run, as root:
`plesk bin extension --exec look-lang post-install.php`.

**"not moved to the new LOOK"** — the site's code does not pass the new release's `lk --check`;
the service was left as it was. Fix the lines listed, then use **Check again** in the panel.

**Domain added but not in the list** — the panel cannot write `domains.json`. Check:
```bash
ls -l /usr/local/psa/var/modules/look-lang/domains.json   # must be psaadm:psaadm
```

**Site 502/403** — is the service running, and is the Apache proxy in place:
```bash
systemctl status look-<domain>; ss -tlnp | grep :<port>
cat /var/www/vhosts/system/<domain>/conf/vhost_ssl.conf   # ProxyPass must be there
```

**Service "failed"** — usually `index.lk` is missing or has a syntax error:
```bash
journalctl -u look-<domain> -n 50 --no-pager
```

---

## Version History

| Version | Changes |
|---|---|
| **1.0.0** | Workspace interface (sidebar: Dashboard/Applications/Logs/Documentation), in-browser **code editor** (Save & Redeploy), live **per-domain monitor** (CPU/RSS/PID/connections/uptime + log tail), **log viewer** (journalctl + copy), system dashboard with live sparklines, **English** UI. State in the Plesk module var directory. |

Later versions removed sudo (root operations go through `sbin/look-ctl`), run each service as the
domain's user, bind HTTP mode to 127.0.0.1 (1.4.2), install LOOK releases side by side with a
per-site `lk --check`, and add the per-site memory limit — see the sections above.

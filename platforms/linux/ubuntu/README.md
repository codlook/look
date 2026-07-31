# LOOK — Linux (systemd)

Works on **Ubuntu / Debian** and **AlmaLinux / Rocky / RHEL** — the same portable
static binary runs on all of them (built on AlmaLinux 8 / glibc 2.28, forward-compatible).

Self-contained installer (binaries bundled in `bin/`). Extract the zip and run
as **root**:

```bash
sudo bash install.sh
```

> On AlmaLinux/RHEL you can alternatively install the dnf-managed RPM
> (`look-lang-1.0.0-1.el8.x86_64.rpm`) for `dnf update look-lang` upgrades, or
> use the **Plesk extension** if the server runs Plesk.

What it does:
- installs `lk` + `lk-fcgi` to `/usr/local/bin`
- creates a sample app at `/var/www/look/index.lk` + `/etc/look/look.env`
- installs a systemd service (`look.service`) on port **9000**, starts & verifies it

Options (env vars): `LOOK_PORT`, `LOOK_WORKERS`, `LOOK_APP_DIR`. See the header of
`install.sh` for details.

- Edit `/var/www/look/index.lk` — the service hot-reloads.
- Service: `systemctl {status|restart|stop} look`
- Logs: `journalctl -u look -f`
- REPL: `lk repl` · Run a file: `lk file.lk`

Requires x86_64 Ubuntu/Debian with systemd. The bundled binary is a portable
static build (static OpenSSL 3.5 LTS, zero dependencies).

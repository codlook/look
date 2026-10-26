# LOOK — Linux (systemd)

Works on **Ubuntu / Debian** and **AlmaLinux / Rocky / RHEL** — the same portable
static binary runs on all of them (built on AlmaLinux 8 / glibc 2.28, forward-compatible).

Self-contained installer (binaries bundled in `bin/`). Extract the zip and run
as **root**:

```bash
sudo bash install.sh
```

> On a Plesk server, use the **Plesk extension** instead.

What it does:
- installs `lk`, `lk-fcgi` and `lk-cgi` to `/usr/local/bin`
- creates a sample app at `/var/www/look/index.lk` + `/etc/look/look.env`
- installs a systemd service (`look.service`, user `look`) on port **9000**, starts & verifies it

It does not install or configure nginx or Apache. The service listens on **every interface**:
if a reverse proxy runs in front of it, add `LOOK_BIND=127.0.0.1` to `/etc/look/look.env` and
`systemctl restart look`, so the port cannot be reached from outside (see
`docs/ubuntu-deployment.md`).

Options (env vars): `LOOK_PORT`, `LOOK_WORKERS`, `LOOK_APP_DIR`. See the header of
`install.sh` for details.

- Edit `/var/www/look/index.lk` — the service hot-reloads.
- Service: `systemctl {status|restart|stop} look`
- Logs: `journalctl -u look -f`
- REPL: `lk repl` · Run a file: `lk file.lk`

Requires x86_64 Linux with systemd. The bundled binary is a portable static build
(static OpenSSL 3.5 LTS, minimal external runtime dependencies).

# Deploying LOOK on Plesk by hand (Apache)

> **Use the Plesk extension if you can.** It writes everything below for each domain, keeps it
> correct across updates and is tested on every release: see
> [plesk-extension-kurulum.md](plesk-extension-kurulum.md). This guide is for servers where the
> extension is not installed. It sets up the same configuration the extension writes.

Requirements: root SSH access, Plesk Obsidian with Apache, AlmaLinux 8/9 or Ubuntu 22.04/24.04.
The same portable binary runs on all of them.

---

## 1. Install the binaries

Download the Linux package from the GitHub release and check it (see
[ubuntu-deployment.md §1](ubuntu-deployment.md#1-install)), then:

```bash
unzip look-lang-linux.zip -d look-lang
install -d -m 755 /opt/look
install -m 755 look-lang/bin/lk look-lang/bin/lk-fcgi look-lang/bin/lk-cgi /opt/look/
/opt/look/lk-fcgi --version
```

`/opt/look` belongs to root and is not writable by the sites. That matters, because the services
run as the sites' users.

---

## 2. The service: one per domain, as the domain's user

Each domain gets its own service and its own local port (9100, 9101, …). **The service runs as
the domain's system user, never as root.** As root, a bug in one site would expose the whole
server, and every other site's files and passwords with it.

Find the domain's user, and keep the environment file outside the web root:

```bash
DOC=/var/www/vhosts/example.com/httpdocs
stat -c '%U' "$DOC"                     # the domain's system user, e.g. example.com_a1b2
install -m 600 -o example.com_a1b2 -g psacln /dev/null /var/www/vhosts/example.com/.look.env
```

`/etc/systemd/system/look-example-com.service`:

```ini
[Unit]
Description=LOOK - example.com
After=network.target mariadb.service mysqld.service

[Service]
Type=simple
User=example.com_a1b2
Group=psacln
NoNewPrivileges=true
PrivateTmp=true
WorkingDirectory=/var/www/vhosts/example.com/httpdocs
EnvironmentFile=/var/www/vhosts/example.com/.look.env
Environment=LOOK_FILE_ROOT=/var/www/vhosts/example.com/httpdocs
Environment=LOOK_SESSION_DIR=/var/www/vhosts/example.com/.look/sessions-look-example-com
ExecStart=/opt/look/lk-fcgi --mode http --bind 127.0.0.1 --port 9100 --workers 4 --script /var/www/vhosts/example.com/httpdocs/index.lk
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

- **`--bind 127.0.0.1`** keeps the port local. Without it, `--mode http` listens on every
  interface, and the site is also reachable at `http://<server ip>:9100/`, bypassing Apache, its
  HTTPS and its rules. `--bind` needs LOOK 1.0.42 or later.
- **Environment file:** systemd reads `KEY=value` lines. A `#` after a value is part of the
  value, not a comment, so put comments on their own lines.
- **Sessions** are kept outside the web root, per site. LOOK creates the directory itself, as the
  site's user, with mode `0700`.

```bash
systemctl daemon-reload
systemctl enable --now look-example-com
ss -tlnp | grep ':9100 '      # must show 127.0.0.1:9100, not 0.0.0.0:9100
```

---

## 3. Apache: proxy the domain to the service

Plesk includes `vhost.conf` and `vhost_ssl.conf` from the domain's `conf` directory. Write the
same lines to both:

```bash
CONF=/var/www/vhosts/system/example.com/conf
cat > "$CONF/vhost.conf" <<'EOF'
ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / http://127.0.0.1:9100/
ProxyPassReverse / http://127.0.0.1:9100/
EOF
cp "$CONF/vhost.conf" "$CONF/vhost_ssl.conf"
/usr/local/psa/admin/bin/httpdmng --reconfigure-domain example.com
```

Point the proxy at `127.0.0.1`, not at `localhost`. `/.well-known` stays with Plesk, so Let's
Encrypt renewals keep working.

To take the client IP and HTTPS from the proxy, add `LOOK_TRUSTED_PROXY=127.0.0.1` to the
environment file.

**FastCGI instead of HTTP:** use
`ExecStart=/opt/look/lk-fcgi --port 9100 --workers 4 --script …` (no `--mode`; FastCGI listens on
`127.0.0.1` by default) and `fcgi://127.0.0.1:9100/` in both `ProxyPass` lines. WebSocket, SSE
and the built-in mail server need `--mode http`.

---

## 4. Check

```bash
systemctl status look-example-com
journalctl -u look-example-com -n 50 --no-pager
curl -s -o /dev/null -w '%{http_code}\n' https://example.com/       # 200 through Apache
```

From another machine, `curl http://<server ip>:9100/` must fail to connect.

---

## 5. Updates

- **App files:** changes to `index.lk` are picked up without a restart.
- **LOOK:** install the new binaries into `/opt/look` as in §1, then
  `systemctl restart look-example-com`. A running service keeps the old binary until it restarts.

---

## Troubleshooting

| Problem | Check |
|---|---|
| 502 / 503 | `systemctl status look-example-com`; the port in `ProxyPass` must match `--port` |
| `203/EXEC` when the service starts | `chmod 755 /opt/look/lk-fcgi` (copying with `scp`/`pscp` can drop the execute bit) |
| The service cannot write its SQLite file or uploads | they must belong to the domain's user: `chown <user>:psacln <file>` |
| The site is reachable on its port from outside | `--bind 127.0.0.1` is missing from `ExecStart` |
| A value from the environment file contains `# …` | move the comment to its own line |

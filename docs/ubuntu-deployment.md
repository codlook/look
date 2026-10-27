# Deploying LOOK on Ubuntu / AlmaLinux

This guide covers Ubuntu 22.04 / 24.04 LTS, Debian, and AlmaLinux / Rocky 8 and 9. The same
portable binary runs on all of them. For the recommended production shape (nginx in front, TLS,
the environment settings that matter) see also [DEPLOYMENT.md](../DEPLOYMENT.md).

---

## 1. Install

Download the Linux package from the GitHub release, check it, and run the installer as root:

```bash
curl -fsSLO https://github.com/codlook/look/releases/latest/download/look-lang-linux.zip
curl -fsSLO https://github.com/codlook/look/releases/latest/download/SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt     # must print: look-lang-linux.zip: OK
unzip look-lang-linux.zip -d look-lang && cd look-lang
sudo bash install.sh
```

Download only from `github.com/codlook/look`. There is no other install address, and no
one-line installer that pipes a download into a root shell.

### What `install.sh` does

- Installs `lk`, `lk-fcgi` and `lk-cgi` to `/usr/local/bin` (from the `bin/` folder in the zip; nothing is downloaded).
- Creates a sample app at `/var/www/look/index.lk`, unless one is already there.
- Writes `/etc/look/look.env`, unless it exists.
- Creates the system user `look` and makes the app directory its own.
- Writes `look.service`, which runs `lk-fcgi --mode http` as `look` on port **9000**. Sessions are kept in `/var/lib/look/sessions`.
- Starts the service and checks that it answers.

### What it does not do

- It does not install or configure **nginx or Apache**.
- It does not set up TLS.
- It does not change the firewall.

It takes no command-line options. The settings are environment variables:

| Variable | Default | |
|---|---|---|
| `LOOK_PORT` | `9000` | HTTP port |
| `LOOK_WORKERS` | `0` | worker threads; `0` = automatic |
| `LOOK_APP_DIR` | `/var/www/look` | app directory |
| `LOOK_BIN_SRC` | the zip's `bin/` | a local folder with the binaries (offline install) |

```bash
sudo LOOK_PORT=9100 LOOK_APP_DIR=/var/www/myapp bash install.sh
```

> **The port is open to the network.** The service listens on every interface, so
> `http://<server ip>:9000/` is reachable from outside unless a firewall blocks it. That is right
> when LOOK itself is the public server; then set the rate limits in
> [DEPLOYMENT.md §4](../DEPLOYMENT.md#4-dos-knobs--and-why-most-are-off-behind-a-proxy).
> **If you put nginx or Apache in front, also do step 2**, otherwise anyone can bypass the proxy,
> and with it TLS and the proxy's rules, by using the port directly.

On a Plesk server, use the Plesk extension (`look-lang-plesk.zip` from the same release) instead.

---

## 2. Behind nginx or Apache: listen on 127.0.0.1 only

Add these lines to `/etc/look/look.env` (LOOK 1.0.42 or later) and restart the service:

```sh
LOOK_BIND=127.0.0.1           # only the proxy on this machine can reach the port
LOOK_TRUSTED_PROXY=127.0.0.1  # take the client IP and https from the proxy's X-Forwarded-* headers
```

```bash
sudo systemctl restart look
ss -tlnp | grep ':9000 '      # must show 127.0.0.1:9000, not 0.0.0.0:9000 or *:9000
```

Writing your own unit file instead? Pass `--bind 127.0.0.1` to `lk-fcgi --mode http`.
`--bind` takes an IPv4 address. A name such as `localhost` is refused, because it may resolve
to the IPv6 address `::1`.

### nginx

```nginx
server {
    listen 80;
    server_name myapp.example.com;

    location / {
        proxy_pass http://127.0.0.1:9000;
        proxy_http_version 1.1;
        proxy_set_header Host              $host;
        proxy_set_header X-Real-IP         $remote_addr;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

For TLS, request-body limits and WebSocket / SSE routes, use the full configuration in
[DEPLOYMENT.md §2](../DEPLOYMENT.md#2-nginx-reverse-proxy-tls--buffering).

### Apache

```bash
sudo a2enmod proxy proxy_http headers      # Ubuntu; on AlmaLinux these modules are loaded by default
```

```apache
<VirtualHost *:80>
    ServerName myapp.example.com
    ProxyRequests Off
    ProxyPreserveHost On
    ProxyPass        / http://127.0.0.1:9000/
    ProxyPassReverse / http://127.0.0.1:9000/
</VirtualHost>
```

In the HTTPS virtual host, also add `RequestHeader set X-Forwarded-Proto "https"`, so that
LOOK marks the session cookie `Secure`.

Point the proxy at `127.0.0.1`, not at `localhost`.

---

## 3. FastCGI mode (optional)

`lk-fcgi` without `--mode http` speaks FastCGI and listens on `127.0.0.1` by default
(`LOOK_FCGI_BIND`). WebSocket, SSE and the built-in mail server work only in `--mode http`.
To use FastCGI, change the service's `ExecStart` (`sudo systemctl edit --full look`):

```ini
ExecStart=/usr/local/bin/lk-fcgi --port 9000 --workers 0 --script /var/www/look/index.lk
```

Then use `fcgi://127.0.0.1:9000/` in Apache's `ProxyPass` (`a2enmod proxy_fcgi`). In nginx, use
`fastcgi_pass 127.0.0.1:9000;` with `include fastcgi_params;`.

---

## 4. Configuration and the database

The service reads `/etc/look/look.env`. Keep secrets there, not in the app directory. The web
server never serves this file.

```sh
DB_DSN=mysql://myapp:password@127.0.0.1/myapp        # MySQL 8 / MariaDB
# DB_DSN=postgres://myapp:password@127.0.0.1/myapp   # PostgreSQL
# DB_DSN=sqlite:///var/www/look/data/app.db          # SQLite (the directory must belong to look)
```

After changing it: `sudo systemctl restart look`.

The service runs as `look`. Anything the app writes must belong to that user: the SQLite file
and its directory, and upload folders. For example: `sudo chown -R look:look /var/www/look`.

> **AlmaLinux / RHEL with SELinux enforcing:** the app's upload directory and SQLite file may
> need an SELinux context (`semanage fcontext` / `restorecon`). Apache in front of LOOK needs
> `setsebool -P httpd_can_network_connect 1` to reach the port. Check the mode with
> `getenforce`.

---

## 5. Running it

```bash
systemctl status look                  # is it running?
journalctl -u look -f                  # logs (errors behind a 500 appear here)
journalctl -u look -n 50 --no-pager    # the last 50 lines
```

- **Editing the app:** changes to `/var/www/look/index.lk` are picked up without a restart.
- **Updating LOOK:** download and check the new release as in step 1, then run `sudo bash install.sh` again, followed by `sudo systemctl restart look`. A service that is already running keeps the old binary until it restarts. The installer keeps your app and `/etc/look/look.env`, but it rewrites `look.service`, so keep your own settings in `look.env`.

---

## Troubleshooting

| Problem | Check |
|---|---|
| 502 / 503 from the proxy | `systemctl status look`; the proxy's port must match `LOOK_PORT` |
| The port is reachable from outside | `LOOK_BIND=127.0.0.1` in `/etc/look/look.env`; check with `ss -tlnp` |
| Every request comes from 127.0.0.1 (rate limiter, logs) | `LOOK_TRUSTED_PROXY=127.0.0.1` |
| Session cookie without `Secure` behind HTTPS | the proxy must send `X-Forwarded-Proto: https`, and `LOOK_TRUSTED_PROXY` must be set |
| The database cannot be opened, or uploads fail | the files belong to `look`; on SELinux, see §4 |

# Deploying LOOK in production

`lk-fcgi --mode http --port 8080 app.lk` is already a complete web server — no
Apache, no XAMPP, no config. For a small or internal deployment you can expose it
directly. For a public production site the recommended shape is **LOOK behind a
reverse proxy (nginx)** that terminates TLS and buffers requests. This guide covers
that shape and the environment knobs that matter.

---

## 1. Run LOOK as a service (systemd)

LOOK listens on a loopback port; the proxy is the only thing exposed to the internet.

`/etc/systemd/system/look-myapp.service`:

```ini
[Unit]
Description=LOOK app (myapp)
After=network.target

[Service]
Type=simple
User=www-data
WorkingDirectory=/var/www/myapp
EnvironmentFile=/etc/look/myapp.env
ExecStart=/usr/bin/lk-fcgi --mode http --port 9101 app.lk
Restart=on-failure
RestartSec=2
# Hardening
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

`/etc/look/myapp.env` — environment lives here, not in the unit or the repo:

```sh
DB_DSN=sqlite:///var/www/myapp/data/app.db
ADMIN_PASSWORD=…            # your app's secrets
LOOK_TRUSTED_PROXY=127.0.0.1  # so X-Forwarded-* from nginx is honoured (see §3)
LOOK_SESSION_SECURE=1         # TLS terminates at the proxy; force Secure on session cookies
```

```bash
systemctl daemon-reload && systemctl enable --now look-myapp
```

---

## 2. nginx reverse proxy (TLS + buffering)

`/etc/nginx/sites-available/myapp.conf`:

```nginx
server {
    listen 443 ssl http2;
    server_name myapp.example.com;

    ssl_certificate     /etc/letsencrypt/live/myapp.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/myapp.example.com/privkey.pem;

    # Request-body limits — the proxy absorbs slow / oversized uploads so they
    # never reach a LOOK worker (see §4).
    client_max_body_size   10m;   # match LOOK_MAX_BODY_SIZE
    client_body_timeout    15s;   # drop a client that dribbles its body
    client_header_timeout  15s;   # …or its headers (slowloris)

    location / {
        proxy_pass http://127.0.0.1:9101;
        proxy_http_version 1.1;

        # proxy_buffering is ON by default — keep it on. nginx reads the whole
        # request (headers + body) before opening the upstream connection, so a
        # slow client ties up cheap nginx buffering, never a LOOK worker.
        proxy_buffering on;
        proxy_request_buffering on;

        proxy_read_timeout 60s;
        proxy_send_timeout 60s;

        # Real client IP — LOOK trusts these ONLY from LOOK_TRUSTED_PROXY (§3).
        proxy_set_header Host              $host;
        proxy_set_header X-Real-IP         $remote_addr;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;   # lets LOOK set Secure cookies
    }
}

# Redirect plain HTTP to HTTPS
server {
    listen 80;
    server_name myapp.example.com;
    return 301 https://$host$request_uri;
}
```

WebSocket / SSE routes need the upgrade headers on their `location`:

```nginx
location /ws {
    proxy_pass http://127.0.0.1:9101;
    proxy_http_version 1.1;
    proxy_set_header Upgrade    $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_read_timeout 3600s;      # long-lived connection
    proxy_buffering off;           # stream frames through, don't buffer
}
```

---

## 3. Real client IP behind the proxy

LOOK treats the **real TCP peer as authoritative** and ignores `X-Forwarded-For` /
`X-Real-IP` unless the connecting address is listed in `LOOK_TRUSTED_PROXY`. Set it to
your proxy's address so `request::ip()`, the rate limiter, and bans see the actual
client instead of `127.0.0.1`:

```sh
LOOK_TRUSTED_PROXY=127.0.0.1        # or the proxy's LAN IP / CIDR
```

Without this, a spoofed `X-Forwarded-For` is correctly ignored — but so is your real
proxy, so every request looks like it comes from the proxy. Set it exactly once, to a
host you control.

---

## 4. DoS knobs — and why most are off behind a proxy

With `proxy_buffering on` (the nginx default), slow-header, slow-body and slow-drip
attacks are absorbed by nginx before LOOK ever sees the request. So on the recommended
reverse-proxy setup, cap these **at the proxy** (`client_body_timeout`,
`client_header_timeout`, `limit_conn`, `limit_req`) and leave LOOK's own limits at
their defaults.

LOOK's built-in limits exist for the **directly internet-facing** case (no proxy):

| Env | Default | What it bounds |
|---|---|---|
| `LOOK_HEADER_TIMEOUT` | `15000` (ms) | Total time to finish request headers (slowloris) |
| `LOOK_BODY_TIMEOUT` | `30000` (ms) | Total time to finish the request body (slow-body) |
| `LOOK_BODY_MIN_RATE` | off | Min average bytes/sec for the body after a 5s grace — **opt-in** |
| `LOOK_HTTP_MAX_CONNS_IP` | off | Max concurrent connections per IP → 429 — **opt-in** |
| `LOOK_MAX_BODY_SIZE` | `10485760` (10 MB) | Max request body → 413 |
| `LOOK_RATE_LIMIT_RPM` / `_GLOBAL_RPM` / `_BURST` | off | Per-IP + global request-rate token bucket |

`LOOK_HTTP_MAX_CONNS_IP` and `LOOK_BODY_MIN_RATE` are **opt-in** on purpose: they key on
the TCP peer IP, and behind a reverse proxy every connection shares the proxy's IP — so
enabling them there would throttle the whole site through one counter. Turn them on only
when LOOK faces the internet directly:

```sh
# Direct-exposure hardening (NO reverse proxy in front):
LOOK_HTTP_MAX_CONNS_IP=50     # one IP can hold at most 50 concurrent connections
LOOK_BODY_MIN_RATE=100        # bytes/sec floor after a 5s grace
LOOK_RATE_LIMIT_RPM=600       # 600 requests/min per IP
```

---

## 5. Concurrency mode: pool (default) vs fiber

LOOK defaults to a **worker pool** (one request per thread), which is fastest for the
common case of short, CPU-bound requests behind a buffering proxy — keep the default.

**Fiber dispatch** (`LOOK_FIBER_DISPATCH=1`) parks a connection on an epoll loop instead
of holding a worker while it waits on I/O, so one worker can interleave many requests
that block on slow/long I/O (`http_client`, a slow `db::query`, streaming). Enable it
**only** when your workload is genuinely I/O-concurrency-bound — many simultaneous
long-waiting requests — where it measurably raised throughput in our tests. For a
CPU-bound app it adds scheduling overhead for no gain. Measure your own workload before
flipping it; it is stable and opt-in either way.

```sh
LOOK_FIBER_DISPATCH=1     # only if your load is many concurrent slow-I/O requests
```

---

## 6. TLS

Terminate TLS at the proxy (nginx above). LOOK's `--mode http` speaks plain HTTP on a
loopback port; the proxy handles certificates, HTTP/2, and renewal (certbot). Outbound
TLS from LOOK (`http::`, `postgresqls://`, `mysqls://`, `rediss://`) verifies the
certificate chain **and** hostname by default — see [SECURITY.md](SECURITY.md).

---

## 7. High-concurrency kernel tuning (c ≥ 5000)

At very high connection rates the kernel's default backlog drops SYNs. On the LOOK host:

`/etc/sysctl.d/99-look.conf`:

```
net.core.somaxconn = 8192
net.ipv4.tcp_max_syn_backlog = 8192
net.ipv4.ip_local_port_range = 15000 64999
```

```bash
sysctl --system
```

Raise the file-descriptor limit for the service (systemd: `LimitNOFILE=200000`).

---

## Checklist

- [ ] `lk-fcgi --mode http` runs as a systemd service on a loopback port
- [ ] nginx terminates TLS and reverse-proxies with `proxy_buffering on`
- [ ] `client_body_timeout` / `client_header_timeout` / `client_max_body_size` set at nginx
- [ ] `LOOK_TRUSTED_PROXY` set to the proxy address (real client IP)
- [ ] `LOOK_SESSION_SECURE=1` (TLS terminates upstream)
- [ ] `LOOK_MAX_BODY_SIZE` matches nginx `client_max_body_size`
- [ ] Per-IP / body-rate limits left off (proxy handles it) — or on, if **no** proxy
- [ ] Fiber dispatch left off unless the workload is I/O-concurrency-bound

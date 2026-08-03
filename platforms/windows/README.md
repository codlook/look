# LOOK for Windows — no installer by design

This zip contains two binaries and nothing else. There is **no `install.ps1`, no
Apache, no XAMPP, no config** — and that is the point.

## Run a web app

```
lk-fcgi.exe --mode http --port 8080
```

That's it. Open <http://localhost:8080>. LOOK's HTTP server is built in (with a
worker pool) — you do not need Apache, nginx, or any web stack to serve LOOK.
It looks for `index.lk` in the current directory; pass a script path to override:

```
lk-fcgi.exe --mode http --port 8080 myapp.lk
```

## Run a script / REPL

```
lk.exe myscript.lk
lk.exe repl
```

## Behind an existing Apache / nginx / IIS?

LOOK is a standard **CGI / FastCGI** binary. Wire it into any web server with
ordinary CGI configuration — the stack (XAMPP, Laragon, WAMP, plain Apache…) does
not matter. LOOK ships the binary; the web-server config is standard and yours.
See the deployment docs at <https://codlook.com/docs>.

## Support tier

Windows x86_64 is **Tier 2**: it builds and the platform-independent core test
suite passes, verified before every release (`release_gate` 7/7, `crypto_vectors`
17/17, and a live `--mode http` check). Not covered on Windows: ThreadSanitizer,
libFuzzer and the differential engine suite (those tools do not exist for MSVC).
The production target is Linux; Docker gives the same environment on any OS.

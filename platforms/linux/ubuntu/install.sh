#!/usr/bin/env bash
# ============================================================================
# LOOK — Ubuntu/Debian single-command install
#
#   1) download look-lang-linux.zip (GitHub Releases):
#      https://github.com/codlook/look/releases/latest/download/look-lang-linux.zip
#   2) unzip look-lang-linux.zip -d look-lang && cd look-lang   (the zip has no top-level folder)
#   3) sudo bash install.sh      # bin/ is embedded in the zip — no download
#
# What it does: installs the binary → sample app + .env → systemd service →
# starts it → verifies. The developer gets a working LOOK server with a single command.
#
# Settings (can be passed through the environment):
#   LOOK_PORT=9000            HTTP port
#   LOOK_WORKERS=0            0 = automatic (CPU*4, max 64)
#   LOOK_APP_DIR=/var/www/look   application directory
#   LOOK_BIN_SRC=<yol>        local binary folder (lk, lk-fcgi) — offline install
#   LOOK_BIN_URL=<url>        optional: root URL of a mirror serving loose binaries (lk, lk-fcgi)
#                             (normally not needed — the release zip comes with bin/)
# ============================================================================
set -euo pipefail

# No options; settings are environment variables. Any option given used to be ignored silently
# (the documentation said --with-nginx: the user thought nginx was installed and the port stayed open to the outside).
# Unknown option → stop without installing anything, saying why.
usage() {
  cat <<'USAGE'
Usage: sudo bash install.sh

Installs lk, lk-fcgi and lk-cgi to /usr/local/bin, a sample app, /etc/look/look.env and a
systemd service (look.service, user look) that runs lk-fcgi --mode http. It does not install or
configure nginx or Apache.

Settings are environment variables (sudo LOOK_PORT=9100 bash install.sh):
  LOOK_PORT=9000           HTTP port
  LOOK_WORKERS=0           worker threads (0 = automatic)
  LOOK_APP_DIR=/var/www/look   app directory
  LOOK_BIN_SRC=<dir>       local folder with the binaries (default: bin/ next to this script)

The service listens on every interface. Behind nginx or Apache, add LOOK_BIND=127.0.0.1 to
/etc/look/look.env and run: systemctl restart look

/etc/look/look.env (mode 0600) also gets a random LOOK_SECRET if it has none: applications that
use cookies need it. On more than one server, copy the same value to every server.
USAGE
}
for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
    *) echo "install.sh: unknown option: $arg (install.sh takes no options; settings are environment variables, see --help)" >&2; exit 2 ;;
  esac
done

LOOK_PORT="${LOOK_PORT:-9000}"
LOOK_WORKERS="${LOOK_WORKERS:-0}"
LOOK_APP_DIR="${LOOK_APP_DIR:-/var/www/look}"
LOOK_BIN_URL="${LOOK_BIN_URL:-}"
BIN_DIR=/usr/local/bin
ETC_DIR=/etc/look

# Self-contained install: if there is a binary in the bin/ folder next to the script, use it
# (once extracted from the release zip, 'sudo bash install.sh' works directly, no download).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${LOOK_BIN_SRC:-}" ] && [ -f "$SCRIPT_DIR/bin/lk-fcgi" ]; then
  LOOK_BIN_SRC="$SCRIPT_DIR/bin"
fi

c_g="\033[32m"; c_b="\033[34m"; c_y="\033[33m"; c_r="\033[31m"; c_0="\033[0m"
say()  { echo -e "${c_b}▶${c_0} $*"; }
ok()   { echo -e "${c_g}✓${c_0} $*"; }
warn() { echo -e "${c_y}!${c_0} $*"; }
die()  { echo -e "${c_r}✗ $*${c_0}" >&2; exit 1; }

# ── 0. Preliminary checks ────────────────────────────────────────────────────────
[ "$(id -u)" = "0" ] || die "root required — run it with 'sudo'."
# curl is needed only on the download path (LOOK_BIN_URL); with the self-contained zip, bin/
# is embedded, so it is mostly never used. If missing, install it in a distro-independent way.
if ! command -v curl >/dev/null 2>&1; then
  # Installing curl is optional/a convenience — it is never used with the self-contained zip (bin/ embedded).
  # Under set -e, with no network 'apt-get install' fails and kills the whole script BEFORE the CRITICAL
  # binary install (sibling of the XAMPP install.ps1 bug). Make it best-effort with '|| true';
  # if curl is really needed (download path), install_bin's own 'die' gives a clear error.
  if   command -v apt-get >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq curl || true
  elif command -v dnf     >/dev/null 2>&1; then dnf install -y -q curl || true
  elif command -v yum     >/dev/null 2>&1; then yum install -y -q curl || true
  fi
fi
ARCH="$(uname -m)"; [ "$ARCH" = "x86_64" ] || warn "tested architecture is x86_64 (yours: $ARCH)"
. /etc/os-release 2>/dev/null || true
say "Starting installation — ${ID:-linux} ${VERSION_ID:-} · port ${LOOK_PORT}"

# ── 1. Fetch the binary ────────────────────────────────────────────────────────
install_bin() { # $1 = name
  local name="$1"
  if [ -n "${LOOK_BIN_SRC:-}" ] && [ -f "$LOOK_BIN_SRC/$name" ]; then
    install -m 0755 "$LOOK_BIN_SRC/$name" "$BIN_DIR/$name"
  elif [ -n "${LOOK_BIN_URL:-}" ]; then
    curl -fsSL "$LOOK_BIN_URL/$name" -o "$BIN_DIR/$name" \
      || die "could not download the binary: $LOOK_BIN_URL/$name"
    chmod +x "$BIN_DIR/$name"
  else
    die "binary not found — run install.sh from inside the extracted release zip (bin/ must be next to it), or set LOOK_BIN_URL/LOOK_BIN_SRC"
  fi
}
say "Installing binaries → $BIN_DIR/{lk, lk-fcgi}"
install_bin lk
install_bin lk-fcgi
# lk-cgi (for Apache CGI) has been in the package since 1.0.41; skipped if an old, manually supplied folder lacks it.
if [ -n "${LOOK_BIN_SRC:-}" ] && [ -f "$LOOK_BIN_SRC/lk-cgi" ]; then
  install -m 0755 "$LOOK_BIN_SRC/lk-cgi" "$BIN_DIR/lk-cgi"
  ok "lk-cgi → $BIN_DIR/lk-cgi"
fi
ok "lk $("$BIN_DIR/lk" --version 2>/dev/null || echo '(installed)')"

# ── 2. Application + .env ────────────────────────────────────────────────────────
mkdir -p "$LOOK_APP_DIR" "$ETC_DIR"
if [ ! -f "$LOOK_APP_DIR/index.lk" ]; then
  cat > "$LOOK_APP_DIR/index.lk" <<'APP'
# LOOK — sample app (edit: /var/www/look/index.lk)
route("GET", "/", fn() => response::json(["ok" => true, "message" => "Hello from LOOK!"]))

route("GET", "/hello/{name}", function($name) {
    response::json(["hello" => $name])
})

route("404", fn() => response::error(404, "Not Found"))
APP
  ok "sample app: $LOOK_APP_DIR/index.lk"
else
  warn "existing app kept: $LOOK_APP_DIR/index.lk"
fi
[ -f "$ETC_DIR/look.env" ] || cat > "$ETC_DIR/look.env" <<ENV
# LOOK environment variables
LOOK_PORT=$LOOK_PORT
LOOK_WORKERS=$LOOK_WORKERS
# Behind nginx or Apache on this machine, listen on 127.0.0.1 only:
# LOOK_BIND=127.0.0.1
# DB_DSN=mysql://user:pass@127.0.0.1/db
# For the built-in mail server (optional):
# LOOK_SMTP_PORT=25
# LOOK_IMAP_PORT=143
# LOOK_MAIL_DIR=/var/mail/look
ENV
# Secret values (DB_DSN password, LOOK_SECRET) are here: only root reads them (systemd reads the EnvironmentFile
# as root, the service gets the values from the environment). It used to be 0644 — every user on the server could read it.
chown root:root "$ETC_DIR/look.env"; chmod 600 "$ETC_DIR/look.env"
# LOOK_SECRET — an application that uses cookies does not start without it (CSRF signature). It is generated from the operating system's cryptographic
# source; it is never written to the screen, the log or the command line. If present it is left alone (it must be the same on every server).
if ! grep -q '^LOOK_SECRET=' "$ETC_DIR/look.env"; then
  secret="$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')"
  [ "${#secret}" = 64 ] || die "could not read 32 random bytes from /dev/urandom"
  { echo "# CSRF signing key for applications that use cookies — the same on every server; keep it secret"
    echo "LOOK_SECRET=$secret"; } >> "$ETC_DIR/look.env"
  unset secret
  ok "LOOK_SECRET created in $ETC_DIR/look.env (mode 0600)"
fi
ok "configuration: $ETC_DIR/look.env"

# ── 3. Service user ─────────────────────────────────────────────────────
# The service used to run as root (no User=): a hole in the application would have given away the whole server.
# Now it has its own system user: the application directory belongs to it (so SQLite and uploaded files can be written).
if ! id look >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/look --no-create-home --shell /usr/sbin/nologin look \
    || die "could not create the system user 'look'"
  ok "system user: look"
fi
chown -R look:look "$LOOK_APP_DIR"

# ── 4. systemd service ────────────────────────────────────────────────────────
UNIT=/etc/systemd/system/look.service
cat > "$UNIT" <<UNITEOF
[Unit]
Description=LOOK application server (--mode http)
After=network.target

[Service]
Type=simple
User=look
Group=look
EnvironmentFile=$ETC_DIR/look.env
# Session files are persistent, in a 0700 directory owned only by the look user (they survive a restart);
# /tmp is private to the service. Low ports (80, 25) can be opened without root.
StateDirectory=look
StateDirectoryMode=0700
Environment=LOOK_SESSION_DIR=/var/lib/look/sessions
PrivateTmp=true
NoNewPrivileges=true
AmbientCapabilities=CAP_NET_BIND_SERVICE
ExecStart=$BIN_DIR/lk-fcgi --mode http --port \${LOOK_PORT} --workers \${LOOK_WORKERS} $LOOK_APP_DIR/index.lk
Restart=on-failure
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
UNITEOF
ok "systemd unit: $UNIT"

# ── 5. Start + verify ───────────────────────────────────────────────────────
HAS_SYSTEMD=0
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then HAS_SYSTEMD=1; fi

if [ "$HAS_SYSTEMD" = "1" ]; then
  say "Starting the service..."
  systemctl daemon-reload
  systemctl enable --now look.service >/dev/null 2>&1 || systemctl restart look.service
  sleep 2
  if curl -fsS "http://127.0.0.1:${LOOK_PORT}/" >/dev/null 2>&1; then
    ok "service is running and responding"
  else
    warn "service started but is not responding — check 'journalctl -u look -n 50'"
  fi
else
  warn "no systemd (container?) — service not installed. Run it manually:"
  echo "    lk-fcgi --mode http --port $LOOK_PORT $LOOK_APP_DIR/index.lk"
fi

# ── 6. Summary ───────────────────────────────────────────────────────────────────
echo
ok "LOOK installed 🎉"
echo -e "   App      : ${c_b}$LOOK_APP_DIR/index.lk${c_0}  (edit it; the service reloads automatically)"
echo -e "   URL      : ${c_b}http://127.0.0.1:${LOOK_PORT}/${c_0}"
[ "$HAS_SYSTEMD" = "1" ] && echo -e "   Service  : ${c_b}systemctl {status|restart|stop} look${c_0}"
echo -e "   Logs     : ${c_b}journalctl -u look -f${c_0}"
echo -e "   REPL     : ${c_b}lk repl${c_0}   ·   Run: ${c_b}lk file.lk${c_0}"

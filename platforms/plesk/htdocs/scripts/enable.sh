#!/bin/bash
# LOOK Language — enable a domain
# Usage: enable.sh <domain> <script> <workers> <mode> <port> [<env-file> [<memory>]]
# Example:    enable.sh look.codlook.com /var/www/.../index.lk 4 fcgi 9100 /var/www/.../.look.env 1G
#   <memory>: the site's memory cap (systemd MemoryMax) — 512M, 2G; "auto" or empty = automatic
#   (25% of the machine's memory, at most 1G, at least 256M); "off" = no cap. LOOK makes 80% of this cap its own
#   limit (LOOK_MEMORY_LIMIT automatic; include/look/mem_budget.h).
#
# The service NEVER runs as root: the domain's system user (the owner of the document
# root) is written into the unit as User=. The environment file is read by that user, not by systemd (root)
# (look-env.sh) — since EnvironmentFile= reads as root, a site owner could symlink to another site's
# .look.env and pull its secrets into their own process.

set -e

DOMAIN="$1"
SCRIPT="$2"
WORKERS="${3:-4}"
MODE="${4:-fcgi}"
PORT="${5:-9100}"
ENV_FILE="${6:-}"
MEMORY="${7:-auto}"

if [ -z "$DOMAIN" ] || [ -z "$SCRIPT" ] || [ -z "$PORT" ]; then
    echo "Usage: $0 <domain> <script> <workers> <mode> <port> [<env-file> [<memory>]]" >&2
    exit 1
fi

# SECURITY (defense in depth): do NOT trust what the caller (PHP) validated. This script
# can be called with a '*' wildcard in sudoers and its content is written into the systemd
# unit below through an UNQUOTED heredoc; without sanitization an arg with an embedded line end injects its own
# directive into the unit and runs a root command via 'sudo systemctl restart'. Whatever its
# source, narrow the args:
DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
SCRIPT=$(printf '%s' "$SCRIPT" | tr -cd 'A-Za-z0-9._/-')
WORKERS=$(printf '%s' "$WORKERS" | tr -cd '0-9'); WORKERS="${WORKERS:-4}"
PORT=$(printf '%s' "$PORT" | tr -cd '0-9')
case "$MODE" in http|fcgi) ;; *) MODE="fcgi" ;; esac
[ -n "$DOMAIN" ] || { echo "ERROR: invalid domain" >&2; exit 1; }
[ -n "$PORT" ]   || { echo "ERROR: invalid port"   >&2; exit 1; }
case "$SCRIPT" in
    /var/www/vhosts/*) ;;
    *) echo "ERROR: script must be under /var/www/vhosts" >&2; exit 1 ;;
esac
case "$SCRIPT" in *..*) echo "ERROR: path traversal" >&2; exit 1 ;; esac
ENV_FILE=$(printf '%s' "$ENV_FILE" | tr -cd 'A-Za-z0-9._/-')
case "$ENV_FILE" in *..*) echo "ERROR: path traversal" >&2; exit 1 ;; esac
# The service is not root, so it cannot open a port below 1024 — say so here instead of systemd's cryptic error
if [ "$PORT" -lt 1024 ] || [ "$PORT" -gt 65535 ]; then
    echo "ERROR: port must be between 1024 and 65535 (the service does not run as root)" >&2; exit 1
fi

# ─── Memory cap (systemd MemoryMax) ──────────────────────────────────────
# A single site must not exhaust the server's memory (MySQL, other sites). The automatic value was measured
# (test server, 2026-11-15): a LookPress site uses 24 MB idle and under three parallel scans; a 1G cap
# leaves ~40x headroom, LOOK makes 80% of it (≈ 819 MiB) its limit, a single request stays within 256 MiB. On a small
# machine (2 GB) a quarter of the machine: 512M. Since it is written into the unit (heredoc), only digits + M/G.
case "$MEMORY" in
    ""|auto)
        ram_mb=$(awk '/^MemTotal:/{print int($2/1024)}' /proc/meminfo 2>/dev/null)
        q=$(( ${ram_mb:-4096} / 4 ))
        [ "$q" -gt 1024 ] && q=1024
        [ "$q" -lt 256 ] && q=256
        MEM_MAX="${q}M" ;;
    off) MEM_MAX="" ;;
    *)
        if ! [[ "$MEMORY" =~ ^([0-9]{1,7})([MG])$ ]]; then
            echo "ERROR: memory limit '$MEMORY' is not a size: write megabytes or gigabytes, for example 512M or 2G (or auto, or off)" >&2; exit 1
        fi
        mb=${BASH_REMATCH[1]}; [ "${BASH_REMATCH[2]}" = G ] && mb=$(( mb * 1024 ))
        if [ "$mb" -lt 128 ]; then
            echo "ERROR: memory limit $MEMORY is below 128M; LOOK cannot serve a site in less" >&2; exit 1
        fi
        MEM_MAX="$MEMORY" ;;
esac
MEM_LINE=""
[ -n "$MEM_MAX" ] && MEM_LINE="MemoryMax=${MEM_MAX}"

# LOOK releases side by side (sbin/look-setup): /opt/look/releases/<version>-<stamp>/, /opt/look/lk* points to the newest
# one. A site is bound to the newest release when its code passes that release's `lk --check`; if it fails, it stays
# on the release it runs (below). In the old layout (no releases) /opt/look/lk-fcgi is a plain file — that works too.
OPT=/opt/look
REL="$OPT/releases"
STATUS_DIR="$OPT/conf/status"
LATEST_FCGI="$(readlink -f "$OPT/lk-fcgi" 2>/dev/null || true)"
if [ -z "$LATEST_FCGI" ] || [ ! -x "$LATEST_FCGI" ]; then
    echo "ERROR: $OPT/lk-fcgi not found or not executable" >&2
    exit 1
fi
LATEST_DIR="$(dirname "$LATEST_FCGI")"
# Release name: the name of the directory under releases, otherwise "<version>-<stamp>" from the binary's --version
rel_name() {
    case "$1" in
        "$REL"/?*) basename "$1" ;;
        *) "$1/lk-fcgi" --version 2>/dev/null | head -1 | sed -n 's/^LOOK \([0-9][0-9A-Za-z.+-]*\) (\([0-9a-f]\{7,\}\)[,)].*/\1-\2/p' ;;
    esac
}
# JSON text (state file): backslash, quote, line end and control characters are escaped
json_str() { printf '%s' "$1" | tr -d '\r' | tr '\000-\010\013-\037' ' ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' | awk 'BEGIN{ORS=""} NR>1{print "\\n"} {print}'; }

# Service name: look.codlook.com -> look-look-codlook-com
SVC_NAME="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]/-/g')"

# Find the domain's document root — fast filesystem path first. The standard Plesk paths
# usually match; when they do, 'plesk bin site --info' (~0.5s) is never called.
# Example: look.codlook.com -> /var/www/vhosts/codlook.com/look.codlook.com/
DOC_ROOT=""
PARENT=$(echo "$DOMAIN" | sed 's/^[^.]*\.//')
if [ -d "/var/www/vhosts/$PARENT/$DOMAIN" ]; then
    DOC_ROOT="/var/www/vhosts/$PARENT/$DOMAIN"
elif [ -d "/var/www/vhosts/$DOMAIN/httpdocs" ]; then
    DOC_ROOT="/var/www/vhosts/$DOMAIN/httpdocs"
fi
# Fallback: Plesk API (only if the fs path does not match — rare)
if [ -z "$DOC_ROOT" ] && command -v plesk &>/dev/null; then
    DOC_ROOT=$(plesk bin site --info "$DOMAIN" 2>/dev/null | grep -i 'document root\|www_root\|httpdocs' | head -1 | awk '{print $NF}')
fi
if [ -z "$DOC_ROOT" ] || [ ! -d "$DOC_ROOT" ]; then
    DOC_ROOT=$(dirname "$SCRIPT")
fi
# DOC_ROOT also goes into the heredoc (WorkingDirectory/LOOK_FILE_ROOT) — even if it comes from the Plesk API output or
# dirname, narrow it one last time (against line-end injection):
DOC_ROOT=$(printf '%s' "$DOC_ROOT" | tr -cd 'A-Za-z0-9._/-')
DOC_ROOT="${DOC_ROOT%/}"
case "$DOC_ROOT" in
    /var/www/vhosts/?*) ;;
    *) echo "ERROR: document root of $DOMAIN not found under /var/www/vhosts" >&2; exit 1 ;;
esac

# The service's user = the owner of the document root (Plesk: the domain's system user).
# If the owner is root or a system account (uid < 1000) do NOT create the service — never fall back to root.
RUN_UID=$(stat -c %u "$DOC_ROOT")
RUN_USER=$(stat -c %U "$DOC_ROOT")
if [ "$RUN_UID" -lt 1000 ] || [ "$(printf '%s' "$RUN_USER" | tr -cd 'A-Za-z0-9._-')" != "$RUN_USER" ]; then
    echo "ERROR: $DOC_ROOT is owned by '$RUN_USER'; a LOOK service runs as the domain's system user, never as root or a system account" >&2
    exit 1
fi
RUN_GROUP=$(id -gn "$RUN_USER")
RUN_GROUP=$(printf '%s' "$RUN_GROUP" | tr -cd 'A-Za-z0-9._-')

# Session files: in the site user's home directory (outside the web root), per site, persistent.
# The service runs with PrivateTmp; sessions used to stay in the private /tmp and were lost on every restart
# (including a plugin update). LOOK creates the directory itself, as the site user, with 0700
# — this script (root) creates/changes nothing in the user's home directory.
# If the home directory is not under the vhost (an unusual setup) no line is written: LOOK uses its own default
# order ($HOME if it belongs to this user, otherwise the service's private /tmp).
RUN_HOME=$(getent passwd "$RUN_USER" | cut -d: -f6 | tr -cd 'A-Za-z0-9._/-')
SESSION_ENV=""
case "$RUN_HOME" in
    /var/www/vhosts/?*) SESSION_ENV="Environment=LOOK_SESSION_DIR=${RUN_HOME%/}/.look/sessions-${SVC_NAME}" ;;
esac

# The script and the environment file must be inside the domain's document root (not another site's file)
case "$SCRIPT" in
    "$DOC_ROOT"/*) ;;
    *) echo "ERROR: the script must be inside the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
esac
if [ -n "$ENV_FILE" ]; then
    # The environment file must be in the domain user's OWN area: the home directory (outside the web root —
    # recommended) or the document root. Only the document root used to be accepted: when the site
    # was stopped, the Apache proxy rule went away and the web root was served as plain files — /.look.env
    # (the database password) could be downloaded.
    case "$ENV_FILE" in
        "$DOC_ROOT"/*) ;;
        "${RUN_HOME%/}"/*) case "$RUN_HOME" in /var/www/vhosts/?*) ;; *) echo "ERROR: the env file must be inside the home of $RUN_USER or the document root of $DOMAIN" >&2; exit 1 ;; esac ;;
        *) echo "ERROR: the env file must be inside the home of $RUN_USER ($RUN_HOME) or the document root of $DOMAIN ($DOC_ROOT)" >&2; exit 1 ;;
    esac
    # Test readability as the service's user (root reads every file)
    if ! runuser -u "$RUN_USER" -- test -f "$ENV_FILE" -a -r "$ENV_FILE"; then
        echo "ERROR: env file not found or not readable by $RUN_USER: $ENV_FILE" >&2; exit 1
    fi
fi

# If the script file does not exist, create a sample application — as the domain user (so root does not
# follow a symlink and write a system file; and the file belongs to the user so they can edit it)
if [ ! -e "$SCRIPT" ] && [ ! -L "$SCRIPT" ]; then
    runuser -u "$RUN_USER" -- mkdir -p "$(dirname "$SCRIPT")"
    runuser -u "$RUN_USER" -- tee "$SCRIPT" > /dev/null << 'INDEXLK'
# LOOK — Plesk domain sample app (edit this file)
route("GET", "/", fn() => response::json(["ok" => true, "app" => "LOOK"]))

route("GET", "/ping", fn() => response::json(["status" => "ok"]))

route("404", fn() => response::error(404, "Not found"))
INDEXLK
    echo "sample app created: $SCRIPT"
fi

# ─── systemd service file ────────────────────────────────────────────────
SVC_FILE="/etc/systemd/system/${SVC_NAME}.service"

install -m 755 "$(dirname "$0")/look-env.sh" /opt/look/look-env
ENV_WRAP=""
if [ -n "$ENV_FILE" ]; then
    # The environment file is read by the service's user: look-env.sh is copied into /opt/look, which belongs to root
    ENV_WRAP="/bin/bash /opt/look/look-env $ENV_FILE"
fi

# ─── LOOK_SECRET ───────────────────────────────────────────────────────────
# LOOK does not start an application that uses cookies without LOOK_SECRET (CSRF signature). If the site has none,
# it is generated here — AS the domain user (runuser): nothing is written to root's
# home directory (a symlink there could have pointed root at any file). 32 bytes from /dev/urandom,
# a 0600 file in a 0700 folder, outside the web root; it is never written to any output. An existing one
# is kept. The outer look-env loads this first, the inner one the site's own file: the site's own
# LOOK_SECRET wins.
case "$RUN_HOME" in
    /var/www/vhosts/?*)
        SECRET_FILE="${RUN_HOME%/}/.look/${SVC_NAME}.secret"
        if runuser -u "$RUN_USER" -- /bin/bash -c '
                umask 077
                mkdir -p "$(dirname "$1")" 2>/dev/null || exit 1
                if [ ! -s "$1" ]; then
                    k=$(od -An -tx1 -N32 /dev/urandom | tr -d " \n")
                    [ ${#k} -eq 64 ] || exit 1
                    printf "LOOK_SECRET=%s\n" "$k" > "$1" || exit 1
                fi
                chmod 600 "$1"' _ "$SECRET_FILE" 2>/dev/null; then
            ENV_WRAP="/bin/bash /opt/look/look-env $SECRET_FILE${ENV_WRAP:+ $ENV_WRAP}"
        else
            echo "WARNING: could not create $SECRET_FILE as $RUN_USER; set LOOK_SECRET in the site's env file (LOOK needs it for applications that use cookies)"
        fi
        ;;
esac

# ─── Which LOOK release? ────────────────────────────────────────────────────
# The newest release's `lk --check` runs against the site's code — as the site's user, in its own directory and with its own
# environment (the same as the service). --check only loads and compiles, it does NOT RUN the program (setup code,
# database migrations do not run; tests/lk_check_no_side_effects_test.sh). If it passes, the newest release; if it fails,
# the site stays on the release it runs and the state file keeps the reason line by line — the panel shows this as "this site receives no
# fixes, security fixes included". A site that has never run before (new)
# is not installed if it fails.
LATEST_REL="$(rel_name "$LATEST_DIR")"
CUR_FCGI=""
if [ -f "$SVC_FILE" ]; then
    CUR_FCGI="$(sed -n 's#^ExecStart=\(.* \)\{0,1\}\(/opt/look/[^ ]*lk-fcgi\) .*#\2#p' "$SVC_FILE" | head -1)"
    # A service written in the extension's unversioned layout ran /opt/look/lk-fcgi: that binary was copied to its own release directory
    # at the first update (look-setup) — the site counts as running it.
    if [ "$CUR_FCGI" = "$OPT/lk-fcgi" ] && [ -s "$OPT/conf/legacy-release" ]; then
        CUR_FCGI="$REL/$(tr -cd 'A-Za-z0-9.+-' < "$OPT/conf/legacy-release")/lk-fcgi"
    fi
fi
# Candidates: first the newest release (bin/, /opt/look/lk*), then the other releases newer than the one the site runs —
# the highest number first (maintenance line, e.g. 1.0.47). The site is moved to the FIRST candidate its code passes:
# a site that cannot yet adapt to a breaking change at least gets the fix releases ("behind"). If none
# passes, it stays on the release it runs ("stayed": it gets no fixes). A candidate without an lk (broken package)
# cannot be checked = did not pass; the protection does not loosen when something goes wrong.
run_check() { # <release directory> → output; return = lk --check's return (127 if there is no lk)
    if [ -x "$1/lk" ]; then
        (cd "$DOC_ROOT" && runuser -u "$RUN_USER" -- env LOOK_FILE_ROOT="$DOC_ROOT" $ENV_WRAP "$1/lk" --check "$SCRIPT" 2>&1)
    else
        echo "LOOK $(rel_name "$1") has no lk ($1/lk), so this site could not be checked against it; install the extension again"
        return 127
    fi
}
ver_of() { printf '%s' "$1" | sed 's/-[0-9a-f]\{7,\}$//'; }   # 1.0.48-7654321 → 1.0.48
CUR_DIR=""; CUR_REL=""
if [ -n "$CUR_FCGI" ] && [ -x "$CUR_FCGI" ]; then CUR_DIR="$(dirname "$CUR_FCGI")"; CUR_REL="$(rel_name "$CUR_DIR")"; fi
newer_than_cur() { # <release name>: is it newer than the one the site runs (yes if what it runs is unknown)
    [ -z "$CUR_REL" ] && return 0
    [ "$(ver_of "$1")" = "$(ver_of "$CUR_REL")" ] && return 1
    [ "$(printf '%s\n%s\n' "$(ver_of "$CUR_REL")" "$(ver_of "$1")" | sort -V | tail -1)" = "$(ver_of "$1")" ]
}
CANDS="$LATEST_DIR"
for r in $(ls -1 "$REL" 2>/dev/null | grep -v '^\.' | sort -V -r); do
    d="$REL/$r"
    [ "$d" = "$LATEST_DIR" ] || [ "$d" = "$CUR_DIR" ] || [ ! -d "$d" ] && continue
    newer_than_cur "$r" && CANDS="$CANDS $d"
done
BIN_DIR=""; LATEST_RC=0; LATEST_OUT=""
for c in $CANDS; do
    rc=0; out="$(run_check "$c")" || rc=$?
    if [ "$c" = "$LATEST_DIR" ]; then LATEST_RC=$rc; LATEST_OUT="$(printf '%s\n' "$out" | head -50)"; fi
    if [ "$rc" = 0 ]; then BIN_DIR="$c"; break; fi
done
CHECK_OUT="$LATEST_OUT"
CHECK_SHORT="$(printf '%s\n' "$CHECK_OUT" | grep -E '^(CHECK |LOOK )' | head -3 | tr '\n' ';' | sed 's/;$//')"
HELD=false; BEHIND=false
if [ -n "$BIN_DIR" ] && [ "$BIN_DIR" = "$LATEST_DIR" ]; then
    :
elif [ -n "$BIN_DIR" ]; then
    BEHIND=true
    echo "WARNING: $DOMAIN runs LOOK $(rel_name "$BIN_DIR"), the newest release its code passes, and gets that line's fixes; LOOK $LATEST_REL needs these lines fixed: $CHECK_SHORT"
elif [ -n "$CUR_DIR" ] && [ "$CUR_DIR" != "$LATEST_DIR" ]; then
    BIN_DIR="$CUR_DIR"; HELD=true
    echo "WARNING: $DOMAIN stays on LOOK $CUR_REL and receives no fixes, security fixes included: it does not pass the check of LOOK $LATEST_REL ($CHECK_SHORT) or of any release between"
elif [ "$LATEST_RC" = 127 ] && [ "$CUR_DIR" = "$LATEST_DIR" ]; then
    # Already on the newest release (restart): nothing is moved — it keeps running
    BIN_DIR="$LATEST_DIR"
    echo "WARNING: $CHECK_OUT"
else
    echo "ERROR: $DOMAIN: the code does not pass the check of LOOK $LATEST_REL; fix these lines and try again:" >&2
    printf '%s\n' "$CHECK_OUT" >&2
    exit 3   # look-setup: do not touch the service (so a running site does not go down), only report
fi
mkdir -p "$STATUS_DIR"; chmod 755 "$OPT/conf" "$STATUS_DIR"
printf '{\n  "domain": "%s",\n  "release": "%s",\n  "latest": "%s",\n  "held": %s,\n  "behind": %s,\n  "check": "%s"\n}\n' \
    "$DOMAIN" "$(rel_name "$BIN_DIR")" "$LATEST_REL" "$HELD" "$BEHIND" \
    "$( { [ "$HELD" = true ] || [ "$BEHIND" = true ]; } && json_str "$CHECK_OUT" )" \
    > "$STATUS_DIR/.$SVC_NAME.json.new" && chmod 644 "$STATUS_DIR/.$SVC_NAME.json.new" \
    && mv -f "$STATUS_DIR/.$SVC_NAME.json.new" "$STATUS_DIR/$SVC_NAME.json"

if [ "$MODE" = "http" ]; then
    # Listens on the local address only: the Apache proxy goes to 127.0.0.1:$PORT (below). It used to listen on all
    # interfaces — the site was also open to the internet on its own port and Apache could be bypassed.
    EXEC_CMD="$BIN_DIR/lk-fcgi --mode http --bind 127.0.0.1 --port $PORT --workers $WORKERS --script $SCRIPT"
else
    EXEC_CMD="$BIN_DIR/lk-fcgi --port $PORT --workers $WORKERS --script $SCRIPT"
fi
EXEC_CMD="${ENV_WRAP:+$ENV_WRAP }$EXEC_CMD"

cat > "$SVC_FILE" << SYSTEMD
[Unit]
Description=LOOK FastCGI - ${DOMAIN}
After=network.target mariadb.service mysqld.service

[Service]
Type=simple
User=${RUN_USER}
Group=${RUN_GROUP}
NoNewPrivileges=true
PrivateTmp=true
WorkingDirectory=${DOC_ROOT}
ExecStart=${EXEC_CMD}
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal
Environment=LOOK_FILE_ROOT=${DOC_ROOT}
${SESSION_ENV}
${MEM_LINE}

[Install]
WantedBy=multi-user.target
SYSTEMD

/bin/systemctl daemon-reload
/bin/systemctl enable "$SVC_NAME" 2>/dev/null || true
/bin/systemctl restart "$SVC_NAME"

# Wait until the port opens (max 5s)
for i in $(seq 1 10); do
    if ss -tlnp | grep -q ":$PORT "; then
        break
    fi
    sleep 0.5
done

# ─── Apache vhost config ───────────────────────────────────────────────────
# Plesk: /var/www/vhosts/system/<domain>/conf/vhost.conf + vhost_ssl.conf
CONF_DIR="/var/www/vhosts/system/${DOMAIN}/conf"
mkdir -p "$CONF_DIR"

if [ "$MODE" = "http" ]; then
    # HTTP Proxy mode
    PROXY_CONF="<FilesMatch \"^\.look\">
    Require all denied
</FilesMatch>
ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / http://127.0.0.1:${PORT}/
ProxyPassReverse / http://127.0.0.1:${PORT}/"
else
    # FastCGI mode (with ProxyPass — mod_proxy_fcgi)
    PROXY_CONF="<FilesMatch \"^\.look\">
    Require all denied
</FilesMatch>
ProxyRequests Off
ProxyPreserveHost On
ProxyPass /.well-known !
ProxyPass / fcgi://127.0.0.1:${PORT}/
ProxyPassReverse / fcgi://127.0.0.1:${PORT}/"
fi

# X-Forwarded-Proto is SET by Apache (not appended): mod_proxy does not add it by itself, and what the client
# sent would pass through unchanged — LOOK would take an HTTP request for HTTPS (session cookie Secure, request::scheme)
# and behave accordingly. mod_proxy adds X-Forwarded-For / X-Forwarded-Host; LOOK reads the rightmost one.
# IfModule: so the configuration does not break (the site does not go down) if mod_headers is missing.
XFP_HTTP="<IfModule mod_headers.c>
    RequestHeader set X-Forwarded-Proto \"http\"
</IfModule>"
XFP_HTTPS="<IfModule mod_headers.c>
    RequestHeader set X-Forwarded-Proto \"https\"
</IfModule>"
NEW_HTTP="$(printf '%s\n%s\n' "$PROXY_CONF" "$XFP_HTTP")"
NEW_HTTPS="$(printf '%s\n%s\n' "$PROXY_CONF" "$XFP_HTTPS")"
# The httpd.conf generated by Plesk includes the two files BY PATH (Include …/vhost.conf). If the content is the same and
# the Include already exists, Apache is running with this configuration: reconfigure is NOT NEEDED. Measured (test.codlook.com,
# 2026-11-18): while the web server reloads at the end of httpdmng --reconfigure-domain, connections go
# unanswered for ~0.5 s (curl 000) + a ~4 s stall — an outage in production on every plugin update/restart.
NEED_RECONF=1
if [ -f "$CONF_DIR/vhost.conf" ] && [ -f "$CONF_DIR/vhost_ssl.conf" ] \
   && [ "$(cat "$CONF_DIR/vhost.conf")" = "$NEW_HTTP" ] && [ "$(cat "$CONF_DIR/vhost_ssl.conf")" = "$NEW_HTTPS" ] \
   && grep -qF "Include \"$CONF_DIR/vhost.conf\"" "$CONF_DIR/httpd.conf" 2>/dev/null \
   && grep -qF "Include \"$CONF_DIR/vhost_ssl.conf\"" "$CONF_DIR/httpd.conf" 2>/dev/null; then
    NEED_RECONF=0
fi
if [ "$NEED_RECONF" = 1 ]; then
    printf '%s\n' "$NEW_HTTP"  > "$CONF_DIR/vhost.conf"
    printf '%s\n' "$NEW_HTTPS" > "$CONF_DIR/vhost_ssl.conf"
fi

# Apache reconfigure (the Plesk way) — runs IN THE BACKGROUND. Plesk reconfigure takes 10-30 s;
# if it ran synchronously it would lock the panel AJAX/time out (the domain would not show).
# The systemd service + vhost config are already ready → reconfigure applies
# them within a few seconds. We return OK immediately and the UI adds the domain to the list at once.
if [ "$NEED_RECONF" = 1 ]; then
nohup bash -c '
    if [ -x /usr/local/psa/admin/bin/httpdmng ]; then
        /usr/local/psa/admin/bin/httpdmng --reconfigure-domain "'"$DOMAIN"'"
    elif [ -x /usr/local/psa/admin/sbin/websrvmng ]; then
        /usr/local/psa/admin/sbin/websrvmng --reconfigure-vhost --vhost-name="'"$DOMAIN"'"
    fi
' >/dev/null 2>&1 &
fi

# The old release ran the service as root: the files it wrote (SQLite, uploads) may have stayed
# owned by root and the service can no longer write to them. There is no automatic chown (walking the site
# tree as root is risky) — it only says so.
ROOT_FILES=$(find "$DOC_ROOT" -xdev -user root 2>/dev/null | head -1000 | wc -l)
if [ "$ROOT_FILES" -gt 0 ]; then
    echo "WARNING: $ROOT_FILES file(s) in $DOC_ROOT are owned by root; the service runs as $RUN_USER and cannot write them (find $DOC_ROOT -user root)"
fi

echo "memory limit: ${MEM_MAX:-off}"
echo "OK:${SVC_NAME}:${PORT}"

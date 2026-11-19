#!/bin/bash
# LOOK Language — load the environment file, then start LOOK
# Usage: look-env <env-file> <command> [args...]
#
# enable.sh installs this as /opt/look/look-env and puts it at the start of ExecStart in the unit.
# Runs as the service's user (User=): reads the file with that user's permissions —
# a symlink to another site's file stops with "Permission denied".
# The format is the same as systemd EnvironmentFile: KEY=VALUE lines, a line starting with '#' is a comment,
# the "..." / '...' around the value is stripped. There are NO inline comments: "KEY=a # b"
# has the value "a # b" — this is a common trap, so a warning is written to the journal.

ENV_FILE="$1"; shift
if ! exec 3< "$ENV_FILE"; then
    echo "LOOK: cannot read the env file $ENV_FILE as $(id -un)" >&2
    exit 1
fi
n=0
while IFS= read -r line <&3 || [ -n "$line" ]; do
    n=$((n + 1))
    line="${line%$'\r'}"
    trimmed="${line#"${line%%[![:space:]]*}"}"
    case "$trimmed" in ''|'#'*|';'*) continue ;; esac
    case "$trimmed" in export[[:space:]]*) trimmed="${trimmed#export}"; trimmed="${trimmed#"${trimmed%%[![:space:]]*}"}" ;; esac
    case "$trimmed" in *=*) ;; *) echo "LOOK: $ENV_FILE line $n has no '=', ignored" >&2; continue ;; esac
    key="${trimmed%%=*}"; key="${key%"${key##*[![:space:]]}"}"
    val="${trimmed#*=}"
    val="${val#"${val%%[![:space:]]*}"}"; val="${val%"${val##*[![:space:]]}"}"
    if ! [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        echo "LOOK: $ENV_FILE line $n: '$key' is not a variable name, ignored" >&2; continue
    fi
    if [ "${#val}" -ge 2 ] && { [[ "$val" == \"*\" ]] || [[ "$val" == \'*\' ]]; }; then
        val="${val:1:${#val}-2}"
    elif [[ "$val" == *[[:space:]]#* ]]; then
        echo "LOOK: $ENV_FILE line $n: the value of $key contains ' #' — inline comments are not supported, the text after # is part of the value" >&2
    fi
    export "$key=$val"
done
exec 3<&-
exec "$@"

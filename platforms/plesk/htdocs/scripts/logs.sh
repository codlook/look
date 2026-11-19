#!/bin/bash
# LOOK Language — service logs (the panel's Log viewer)
# Usage: logs.sh <service> [lines]
# Security: only look-* services; journalctl runs via look-ctl (root, callSbin).
SVC="$1"
N="${2:-200}"

# Only valid LOOK service names
case "$SVC" in
    look-*) ;;
    *) echo "ERROR: invalid service name: $SVC" >&2; exit 1 ;;
esac
# Limit the line count
case "$N" in
    ''|*[!0-9]*) N=200 ;;
esac
[ "$N" -gt 1000 ] && N=1000

journalctl -u "$SVC" -n "$N" --no-pager -o short-iso 2>&1

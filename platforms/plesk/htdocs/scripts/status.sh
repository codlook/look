#!/bin/bash
# LOOK Language — Service status
# Usage: status.sh <svc_name>
# Cikti:    active|inactive|failed|unknown [PID]

SVC_NAME="$1"

if [ -z "$SVC_NAME" ]; then
    echo "unknown"
    exit 0
fi

# SECURITY (consistent with logs/monitor/disable): query only look-* services.
SVC_NAME=$(printf '%s' "$SVC_NAME" | tr -cd 'A-Za-z0-9.-')
case "$SVC_NAME" in look-*) ;; *) echo "unknown"; exit 0 ;; esac

STATE=$(/bin/systemctl is-active "$SVC_NAME" 2>/dev/null || echo "unknown")

if [ "$STATE" = "active" ]; then
    PID=$(/bin/systemctl show -p MainPID "$SVC_NAME" 2>/dev/null | cut -d= -f2)
    echo "active:${PID}"
else
    echo "$STATE"
fi

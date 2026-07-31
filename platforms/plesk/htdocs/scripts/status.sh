#!/bin/bash
# LOOK Language — Servis durumu
# Kullanim: status.sh <svc_name>
# Cikti:    active|inactive|failed|unknown [PID]

SVC_NAME="$1"

if [ -z "$SVC_NAME" ]; then
    echo "unknown"
    exit 0
fi

STATE=$(sudo /bin/systemctl is-active "$SVC_NAME" 2>/dev/null || echo "unknown")

if [ "$STATE" = "active" ]; then
    PID=$(sudo /bin/systemctl show -p MainPID "$SVC_NAME" 2>/dev/null | cut -d= -f2)
    echo "active:${PID}"
else
    echo "$STATE"
fi

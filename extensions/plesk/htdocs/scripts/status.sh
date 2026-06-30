#!/bin/bash
# LOOK Language — Domain servis durumu
# Kullanim: status.sh <domain>
# Cikti: JSON
set -euo pipefail

DOMAIN="$1"
SVC_NAME="look-$(echo "$DOMAIN" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9-]/-/g')"
LOG_DIR="/var/log/look"
LOG_FILE="${LOG_DIR}/${DOMAIN}.log"

# systemd durumu
STATE=$(systemctl is-active "$SVC_NAME" 2>/dev/null || echo "unknown")

# Uptime
UPTIME=""
if [ "$STATE" = "active" ]; then
    UPTIME=$(systemctl show "$SVC_NAME" --property=ActiveEnterTimestamp 2>/dev/null \
        | grep -oP '=\K.+' | xargs -I{} date -d {} +%s 2>/dev/null || echo "")
    if [ -n "$UPTIME" ]; then
        NOW=$(date +%s)
        DIFF=$((NOW - UPTIME))
        MINS=$((DIFF / 60))
        HOURS=$((MINS / 60))
        DAYS=$((HOURS / 24))
        if [ $DAYS -gt 0 ]; then
            UPTIME="${DAYS}g $((HOURS % 24))s"
        elif [ $HOURS -gt 0 ]; then
            UPTIME="${HOURS}s $((MINS % 60))d"
        else
            UPTIME="${MINS}d"
        fi
    fi
fi

# RAM kullanimi
RAM=""
PID=$(systemctl show "$SVC_NAME" --property=MainPID 2>/dev/null | grep -oP '=\K\d+' || echo "0")
if [ "$PID" -gt 0 ] 2>/dev/null; then
    KB=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ' || echo "")
    if [ -n "$KB" ] && [ "$KB" -gt 0 ] 2>/dev/null; then
        MB=$(echo "scale=1; $KB / 1024" | bc 2>/dev/null || echo "")
        RAM="${MB} MB"
    fi
fi

# Son 50 satir log
LOGS=""
if [ -f "$LOG_FILE" ]; then
    LOGS=$(tail -50 "$LOG_FILE" 2>/dev/null || "")
else
    LOGS=$(journalctl -u "$SVC_NAME" -n 50 --no-pager 2>/dev/null || "")
fi

# JSON cikarma (jq yoksa elle)
printf '{"state":"%s","uptime":"%s","ram":"%s","logs":%s}\n' \
    "$STATE" \
    "$UPTIME" \
    "$RAM" \
    "$(echo "$LOGS" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))' 2>/dev/null \
       || echo '"(log okunamadi)"')"

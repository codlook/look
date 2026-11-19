#!/bin/bash
# LOOK Language — live service monitor (the panel's Monitor modal)
# Usage: monitor.sh <servis>
# Output: key=value lines (parsed by PHP). Runs through look-ctl (root, callSbin).
SVC="$1"
case "$SVC" in
    look-*) ;;
    *) echo "ERROR: invalid service name: $SVC" >&2; exit 1 ;;
esac

# systemd status fields (already key=value)
systemctl show "$SVC" -p ActiveState,SubState,MainPID,NRestarts,ActiveEnterTimestamp 2>/dev/null

PID="$(systemctl show -p MainPID --value "$SVC" 2>/dev/null)"
if [ -n "$PID" ] && [ "$PID" != "0" ]; then
    # CPU% · RSS(KB) · elapsed time (s)
    read -r CPU RSS ETIMES < <(ps -o %cpu=,rss=,etimes= -p "$PID" 2>/dev/null)
    echo "PCPU=${CPU:-0}"
    echo "RSS=${RSS:-0}"
    echo "ETIMES=${ETIMES:-0}"
    # Listening port (belonging to this PID) + number of established connections
    PORT="$(ss -tlnp 2>/dev/null | grep "pid=$PID," | grep -oE ':[0-9]+ ' | head -1 | tr -d ': ')"
    echo "PORT=${PORT}"
    if [ -n "$PORT" ]; then
        CONN="$(ss -tn state established "sport = :$PORT" 2>/dev/null | grep -c ':')"
        echo "CONN=${CONN:-0}"
    fi
fi

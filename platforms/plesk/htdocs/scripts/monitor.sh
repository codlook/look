#!/bin/bash
# LOOK Language — servis canli monitoru (panel Monitör modali)
# Kullanim: monitor.sh <servis>
# Cikti: key=value satirlari (PHP parse eder). sudo ile root calisir.
SVC="$1"
case "$SVC" in
    look-*) ;;
    *) echo "gecersiz servis: $SVC" >&2; exit 1 ;;
esac

# systemd durum alanlari (zaten key=value)
systemctl show "$SVC" -p ActiveState,SubState,MainPID,NRestarts,ActiveEnterTimestamp 2>/dev/null

PID="$(systemctl show -p MainPID --value "$SVC" 2>/dev/null)"
if [ -n "$PID" ] && [ "$PID" != "0" ]; then
    # CPU% · RSS(KB) · gecen sure(sn)
    read -r CPU RSS ETIMES < <(ps -o %cpu=,rss=,etimes= -p "$PID" 2>/dev/null)
    echo "PCPU=${CPU:-0}"
    echo "RSS=${RSS:-0}"
    echo "ETIMES=${ETIMES:-0}"
    # Dinlenen port (bu PID'e ait) + established baglanti sayisi
    PORT="$(ss -tlnp 2>/dev/null | grep "pid=$PID," | grep -oE ':[0-9]+ ' | head -1 | tr -d ': ')"
    echo "PORT=${PORT}"
    if [ -n "$PORT" ]; then
        CONN="$(ss -tn state established "sport = :$PORT" 2>/dev/null | grep -c ':')"
        echo "CONN=${CONN:-0}"
    fi
fi

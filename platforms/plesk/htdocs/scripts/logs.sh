#!/bin/bash
# LOOK Language — servis loglari (panel Log goruntuleyici)
# Kullanim: logs.sh <servis> [satir]
# Guvenlik: yalnizca look-* servisleri; sudo ile root olarak journalctl calisir.
SVC="$1"
N="${2:-200}"

# Yalnizca gecerli LOOK servis adlari
case "$SVC" in
    look-*) ;;
    *) echo "gecersiz servis: $SVC" >&2; exit 1 ;;
esac
# Satir sayisi sinirla
case "$N" in
    ''|*[!0-9]*) N=200 ;;
esac
[ "$N" -gt 1000 ] && N=1000

journalctl -u "$SVC" -n "$N" --no-pager -o short-iso 2>&1

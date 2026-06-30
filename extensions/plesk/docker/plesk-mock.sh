#!/bin/bash
# Plesk CLI mock — sık kullanılan komutları simüle eder

case "$*" in
    *"site --list"*)
        # Kurulu domain listesi
        echo "look.tobiyo.com"
        echo "tobiyo.com"
        ;;
    *"bin site --list"*)
        echo "look.tobiyo.com"
        echo "tobiyo.com"
        ;;
    *"sbin httpdmng"*|*"sbin websrvmng"*)
        echo "[mock] httpdmng: nginx/apache reload simulated"
        ;;
    *"bin extension --list"*)
        echo "look-lang"
        ;;
    *)
        echo "[mock plesk] $*"
        ;;
esac

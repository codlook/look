#!/usr/bin/env bash
# Under CGI, expired session files are removed too — once per interval, after the response.
#
# 1.0.39 kept the time of the last sweep in the process and swept in a background thread. Under
# CGI every request is a new process that lives a few milliseconds: the thread died with the
# process before it swept (measured: after the TTL, 20 more requests, still all 320 files).
# Shared hosting with CGI was the place the disk-filling attack hurt most. Now the time of the
# last sweep is kept in <session dir>/.gc-stamp (locked): one sweep per interval for all
# processes, and under CGI it runs after the response has been sent (stdout is closed first).
# Usage: session_gc_cgi_test.sh <lk-cgi>   (needs mkfifo, xargs)
set -u
# LOOK 2: çerez kullanan uygulama LOOK_SECRET olmadan başlamaz (CSRF imzası) — testin kendi anahtarı.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
CGI="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -m 0700 "$T/s"
cat > "$T/app.lk" <<'LK'
route("GET", "/csrf", function() { session::start(); session::set("csrf", "token"); return response::text("ok") })
LK
req() {   # bir CGI isteği (Apache'nin verdiği ortamla); çıktı = yanıt
    (cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET="$LOOK_SECRET" REQUEST_METHOD=GET SCRIPT_FILENAME="$T/app.lk" PATH_INFO=/csrf \
        REQUEST_URI=/csrf GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" LOOK_SESSION_TTL="$TTL" "$CGI" 2>/dev/null)
}
files() { find "$T/s" -name 'look_sess_*' | wc -l; }
fail=0
TTL=4
for i in $(seq 1 300); do req >/dev/null; done
before=$(files)
sleep 5
for i in $(seq 1 20); do req >/dev/null; done
sleep 1
after=$(files)
if [ "$before" -ge 100 ] && [ "$after" -le 25 ]; then echo "  OK   CGI: $before files after 300 requests, $after after the TTL"
else echo "  FAIL CGI: $before files after 300 requests, still $after after the TTL"; fail=1; fi

# Yanıt taramayı beklemez: 100 000 süresi dolmuş dosya varken yanıt (çıktının sonu, EOF) taramadan
# önce gelir. Bir FIFO ile ölçülür: $(...) EOF'tan sonra sürecin bitmesini de beklediği için yanıltır.
( cd "$T/s" && seq -f "look_sess_%032g" 1 100000 | xargs touch && find . -name 'look_sess_*' -exec chmod 600 {} + \
  && find . -name 'look_sess_*' -exec touch -d '1 hour ago' {} + && touch -d '1 hour ago' .gc-stamp )
mkfifo "$T/out"
s=$(date +%s%N)
( cd "$T" && exec env -i PATH=/usr/bin:/bin LOOK_SECRET="$LOOK_SECRET" REQUEST_METHOD=GET SCRIPT_FILENAME="$T/app.lk" PATH_INFO=/csrf \
    REQUEST_URI=/csrf GATEWAY_INTERFACE=CGI/1.1 LOOK_SESSION_DIR="$T/s" LOOK_SESSION_TTL="$TTL" "$CGI" > "$T/out" 2>/dev/null ) & cp=$!
cat "$T/out" > "$T/body"; eof=$(( ($(date +%s%N) - s) / 1000000 ))
wait $cp; done_ms=$(( ($(date +%s%N) - s) / 1000000 ))
left=$(files)
grep -q ok "$T/body" || { echo "  FAIL no response: '$(head -c 80 "$T/body")'"; fail=1; }
if [ "$left" -le 5 ] && [ "$eof" -lt "$done_ms" ]; then
    echo "  OK   100000 expired files removed by one request; its response was complete after ${eof} ms, the sweep after ${done_ms} ms"
else echo "  FAIL 100000 expired files: $left left; response after ${eof} ms, process end after ${done_ms} ms"; fail=1; fi

# Aralık içinde yeniden tarama yok: damga dosyası değişmez
TTL=86400
req >/dev/null; m1=$(stat -c %Y "$T/s/.gc-stamp")
for i in $(seq 1 30); do req >/dev/null; done; m2=$(stat -c %Y "$T/s/.gc-stamp")
m=$(stat -c %a "$T/s/.gc-stamp")
if [ "$m1" = "$m2" ] && [ "$m" = 600 ]; then echo "  OK   one sweep per interval for all processes (stamp unchanged over 30 requests, mode $m)"
else echo "  FAIL the stamp changed within the interval ($m1 → $m2) or has mode $m"; fail=1; fi

[ $fail = 0 ] && echo "PASS: CGI removes expired session files, after the response" || echo "FAIL: CGI session cleanup"
exit $fail

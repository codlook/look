#!/usr/bin/env bash
set +e
cd /look/cpp
pass=0; fail=0
chk(){ if [ "$2" = "$3" ]; then echo "  PASS $1"; pass=$((pass+1)); else echo "  FAIL $1: bekl '$3' geldi '$2'"; fail=$((fail+1)); fi; }

echo "=== differential (temiz kosum) ==="
bash tests/differential_test.sh ./build/lk ./build/lk-fcgi 2>&1 | grep -E '^PASS:|^FAIL' | head -1

# HTTP test app: tum web-fix'leri
cat > /tmp/verify_app.lk <<'LK'
use cache;
route("GET","/health", function(){ print("ok") })
route("POST","/x", function(){ print("POST-OK") })
route("GET","/r5/{id}", function(){ return response::text("r5") })
route("GET","/int64", function(){ $a=9007199254740992+1; $b=9007199254740992; return response::text(($a==$b) . "," . ($a>$b)) })
route("GET","/cacheiso", function(){ cache::set("t",[1,2,3]); $a=cache::get("t"); push($a,9); return response::text(count(cache::get("t"))) })
LK
./build/lk-fcgi --mode http --port 7405 --workers 4 /tmp/verify_app.lk >/tmp/vapp.log 2>&1 &
pid=$!
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:7405/health 2>/dev/null | grep -q ok && break; sleep 0.3; done

echo "=== HTTP web-fix'leri ==="
chk "routing /r5"      "$(curl -s --max-time 2 http://127.0.0.1:7405/r5/9)" "r5"
chk "int64 (route)"    "$(curl -s --max-time 2 http://127.0.0.1:7405/int64)" "false,true"
chk "cache-izo (route)" "$(curl -s --max-time 2 http://127.0.0.1:7405/cacheiso)" "3"
# normal POST -> 200
norm=$(printf 'POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n' | python3 -c "import socket,sys; s=socket.create_connection(('127.0.0.1',7405)); s.sendall(sys.stdin.buffer.read()); print(s.recv(60).split(b'\r\n')[0].decode())")
chk "normal POST 200"  "$norm" "HTTP/1.1 200 OK"
# bare-LF smuggling -> reddedilmeli (bos)
smug=$(printf 'POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nX-F: j\nContent-Length: 44\r\nConnection: close\r\n\r\n' | python3 -c "import socket,sys; s=socket.create_connection(('127.0.0.1',7405)); s.sendall(sys.stdin.buffer.read()); 
import sys
try: r=s.recv(60)
except: r=b''
print('EMPTY' if not r else r.split(b'\r\n')[0].decode())")
chk "bare-LF reddedildi" "$smug" "EMPTY"

# eszamanli cache izolasyon (race yoksa hepsi 4)
echo "=== eszamanli cache (channel/cache izolasyon, race yok) ==="
res=$(for i in $(seq 1 100); do curl -s --max-time 3 http://127.0.0.1:7405/cacheiso & done; wait | sort | uniq -c | tr -s ' ')
echo "  cacheiso x100 dagilim:$res (hepsi 3 = izolasyon+crash yok)"

kill $pid 2>/dev/null
echo ""
echo "=== HTTP SONUC: $pass PASS, $fail FAIL ==="

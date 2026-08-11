#!/usr/bin/env bash
# WebSocket close + fd-sızıntısı fonksiyonel guard'ı (SSE sse_close_test.sh ikizi, TSan-siz).
# Ham-socket handshake (python3 stdlib — CI'da websocket-client YOK) + /proc/PID/fd sayacı.
# Kapsam: handshake 101 · echo · istemci close-frame → server close + fd serbest · regresyon.
# NOT: WS EWOULDBLOCK (yavaş-istemci write-drop) bu fonksiyonel testin KAPSAMI DIŞI — ayrı
# yük/backpressure testi ister (send_raw blocking; kayıtta).
set -u
FCGI="${1:-./build/lk-fcgi}"
PORT="${2:-7769}"
TMP=$(mktemp -d)
SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; sleep 1; [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
command -v python3 >/dev/null 2>&1 || { echo "  (atlandi: python3 gerekli)"; exit 0; }
[ -d /proc ] || { echo "  (atlandi: /proc gerekli)"; exit 0; }
fail=0

cat > "$TMP/app.lk" <<'LKEOF'
route("WS", "/ws", function($conn) {
    ws::on($conn, "message", function($m) { ws::send($conn, "echo:" . $m) })
})
route("GET", "/", function() { print("ok") })
LKEOF

"$FCGI" --mode http --port "$PORT" "$TMP/app.lk" >"$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null && break; sleep 0.25; done
sleep 1
base=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)

# Ham-socket WS istemcisi: handshake→echo→close; bağlıyken ve close sonrası fd döner.
out=$(PORT="$PORT" python3 - "$SRV" "$base" <<'PYEOF'
import socket, os, base64, struct, sys, time
pid, base = sys.argv[1], int(sys.argv[2])
port = int(os.environ["PORT"])
def fdcount():
    try: return len(os.listdir("/proc/%s/fd" % pid))
    except Exception: return -1
def masked(op, payload=b""):
    m = os.urandom(4)
    mp = bytes(b ^ m[i % 4] for i, b in enumerate(payload))
    return bytes([0x80 | op, 0x80 | len(payload)]) + m + mp
s = socket.create_connection(("127.0.0.1", port), timeout=5); s.settimeout(5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
resp = s.recv(1024)
ok101 = b"101" in resp.split(b"\r\n")[0]
s.sendall(masked(0x1, b"ping"))                 # text frame
fr = s.recv(256)                                # server echo (unmasked)
echoed = b"echo:ping" in fr
time.sleep(0.3); conn = fdcount()               # bağlıyken fd
s.sendall(masked(0x8))                          # close frame
try: tail = s.recv(256)                          # server close frame + EOF
except Exception: tail = b""
s.close(); time.sleep(1); after = fdcount()
print("OK101=%s ECHO=%s CONN=%d BASE=%d AFTER=%d" % (ok101, echoed, conn, base, after))
PYEOF
)
echo "  $out"
ok101=$(echo "$out" | grep -o 'OK101=True'); echoed=$(echo "$out" | grep -o 'ECHO=True')
conn=$(echo "$out" | sed -n 's/.*CONN=\([0-9]*\).*/\1/p'); after=$(echo "$out" | sed -n 's/.*AFTER=\([0-9]*\).*/\1/p')

# Sanity (Kural 3): handshake + echo gerçekten oldu mu (test WS yolunu egzersiz etti mi)
if [ -z "$ok101" ] || [ -z "$echoed" ] || [ "${conn:-0}" -ne $((base+1)) ]; then
  echo "  FAIL sanity: WS bağlanmadı/echo yok (fd $base→${conn:-?})"; fail=1
else echo "  OK handshake 101 + echo:ping + bağlı fd=$conn"; fi
# Asıl: close sonrası fd base'e döndü mü (sızıntı yok)
if [ "${after:-99}" -eq "$base" ]; then echo "  OK close sonrası fd base'e döndü ($after) — sızıntı yok"
else echo "  FAIL: close sonrası fd=$after (base=$base) — SIZINTI"; fail=1; fi

# Regresyon: 3 ani-kopma (close-frame'siz abandon) → sızıntı/crash yok
python3 - "$PORT" <<'PYEOF'
import socket, os, base64
port = int(__import__("sys").argv[1])
for _ in range(3):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    s.recv(256); s.close()   # ani kopma
PYEOF
sleep 1
recyc=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
if kill -0 "$SRV" 2>/dev/null && [ "$recyc" -le $((base+1)) ]; then echo "  OK ani-kopma 3-döngü: fd=$recyc, server ayakta"
else echo "  FAIL: ani-kopma regresyonu (fd=$recyc, server $(kill -0 $SRV 2>/dev/null && echo ayakta || echo ÇÖKTÜ))"; fail=1; fi

[ $fail = 0 ] && echo "PASS: WS close + fd-sizintisi" || echo "FAIL: WS close + fd-sizintisi"
exit $fail

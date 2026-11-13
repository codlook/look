#!/bin/bash
# Paylaşılan büyük sözlüğü (app::set) birçok işçi iş parçacığı AYNI ANDA ilk kez okur: anahtar dizini
# (include/look/assoc_index.h) okuma yolunda kurulur — kurulum yarışsız olmalı (TSan derlemesiyle de
# koşulur) ve her istek doğru değeri görmeli.
# Usage: shared_map_concurrent_test.sh <lk-fcgi> [port]   (needs python3, curl)
set -u
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
LKF="$1"; PORT="${2:-7931}"
T=$(mktemp -d); pid=""; trap '{ [ -n "$pid" ] && kill $pid; wait; } 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lk" <<'LK'
use json
use string
// json::decode ile kurulan sözlüğün dizini yok: ilk okuyan istekler onu aynı anda kurar.
$parts = []
for ($i = 0; $i < 2000; $i++) { push($parts, "\"key_" . $i . "\":" . $i) }
app::set("decoded", json::decode("{" . string::join($parts, ",") . "}"))
// döngüyle kurulan sözlüğün dizini yazarken kurulmuştur: yalnız okunur.
$built = []
for ($i = 0; $i < 2000; $i++) { $built["key_" . $i] = $i }
app::set("built", $built)
route("GET", "/r", function() {
    $d = app::get("decoded")
    $b = app::get("built")
    $i = int(request::get("i"))
    $s = 0
    for ($j = 0; $j < 50; $j++) {
        $k = "key_" . (($i + $j) % 2000)
        $s += $d[$k] + $b[$k]
    }
    if ($d["nope"] != null) { $s = -1 }
    return response::text("" . $s)
})
LK
wait_up() { for i in $(seq 1 100); do curl -s --max-time 1 "http://127.0.0.1:$1/health" | grep -q ok && return 0; sleep 0.2; done; return 1; }
"$LKF" --mode http --port "$PORT" --workers 8 "$T/app.lk" >"$T/srv.log" 2>&1 &
pid=$!
wait_up "$PORT" || { echo "FAIL: server did not start"; cat "$T/srv.log"; exit 1; }
fail=0
python3 - "$PORT" <<'PY' || fail=1
import http.client, sys, threading
port = int(sys.argv[1]); N, ROUNDS = 32, 20
bad = []; errs = []
gate = threading.Barrier(N)
def want(i): return 2 * sum((i + j) % 2000 for j in range(50))
def client(t):
    try:
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
        gate.wait()   # ilk istekler aynı anda: dizini birlikte kurmaya çalışırlar
        for r in range(ROUNDS):
            i = t * 97 + r * 13
            c.request("GET", "/r?i=%d" % i); resp = c.getresponse()
            got = resp.read().decode()
            if got != str(want(i)): bad.append((i, got)); return
    except Exception as e:
        errs.append((t, repr(e)))
ts = [threading.Thread(target=client, args=(t,)) for t in range(N)]
[t.start() for t in ts]; [t.join() for t in ts]
if bad or errs:
    print("FAIL: shared map reads: %d wrong, %d errors; e.g. %s %s" % (len(bad), len(errs), bad[:3], errs[:3]))
    sys.exit(1)
print("PASS: %d concurrent clients x %d requests read two shared 2000-key maps correctly" % (N, ROUNDS))
PY
kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=""
if grep -qE "ThreadSanitizer|AddressSanitizer|LeakSanitizer|runtime error:" "$T/srv.log"; then
    echo "FAIL: sanitizer report in the server log:"; grep -A30 -m1 -E "ThreadSanitizer|AddressSanitizer|LeakSanitizer|runtime error:" "$T/srv.log"; fail=1
fi
exit $fail

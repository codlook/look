#!/bin/bash
# Docker içinde çalışır
# look-fcgi interpreter vs VM RPS karşılaştırması

BINARY=/test/look-fcgi
SCRIPT=/test/index.lk
PORT=9003
WORKERS=4

echo "=== LOOK RPS Test ==="
echo "Script: $SCRIPT"
echo "Workers: $WORKERS"
echo ""

run_test() {
    local label=$1
    local bytecode=$2

    echo "--- $label (LOOK_BYTECODE=$bytecode) ---"

    # Önceki instance'ı temizle
    pkill -f "look-fcgi.*$PORT" 2>/dev/null
    sleep 0.5

    # look-fcgi başlat
    LOOK_BYTECODE=$bytecode $BINARY --mode http --port $PORT \
        --workers $WORKERS --script $SCRIPT > /tmp/look-$bytecode.log 2>&1 &
    LOOK_PID=$!
    sleep 2

    # Smoke test
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:$PORT/router)
    if [ "$HTTP_CODE" != "200" ]; then
        echo "FAIL: /router $HTTP_CODE (log: $(tail -3 /tmp/look-$bytecode.log))"
        kill $LOOK_PID 2>/dev/null
        return
    fi
    echo "Smoke: /router → $HTTP_CODE ✓"

    # Warmup
    wrk -t2 -c20 -d3s http://127.0.0.1:$PORT/router > /dev/null 2>&1

    # RPS ölçümü — /router (DB yok)
    echo "Measuring /router (no DB, 30s)..."
    wrk -t4 -c50 -d30s http://127.0.0.1:$PORT/router 2>&1 | grep -E "Requests/sec|Latency|requests in"

    kill $LOOK_PID 2>/dev/null
    sleep 1
    echo ""
}

run_test "Interpreter" 0
run_test "Bytecode VM" 1

echo "=== DONE ==="

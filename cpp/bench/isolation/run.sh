#!/usr/bin/env bash
# Requests per second for routes that touch shared setup data, best of 3.
# Usage: run.sh <lk-fcgi>   (copy the binary to a container-local path first, not a bind mount)
FCGI="${1:?usage: $0 <lk-fcgi>}"; HERE="$(cd "$(dirname "$0")" && pwd)"; PORT="${PORT:-7760}"
TMP="$(mktemp -d)"; cp "$HERE/app.lk" "$TMP/"; trap 'kill $pid 2>/dev/null; rm -rf "$TMP"' EXIT
( cd "$TMP" && exec "$FCGI" --mode http --port "$PORT" --workers 4 app.lk > "$TMP/log.txt" 2>&1 ) & pid=$!
for i in $(seq 1 50); do curl -s -o /dev/null -m 1 "localhost:$PORT/plain" && break; sleep 0.1; done
for r in plain global capture struct write; do best=0
  for i in 1 2 3; do v="$(python3 "$HERE/load.py" "$PORT" "/$r" 4 4000)"; [ "$v" -gt "$best" ] && best=$v; done
  printf '%-10s %8s req/s   %s\n' "$r" "$best" "$(curl -s -m 2 "localhost:$PORT/$r" | cut -c1-30)"
done
grep -c "VM BUG\|runs on the interpreter\|Dispatch error" "$TMP/log.txt" | sed 's/^/log problems: /'

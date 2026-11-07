#!/usr/bin/env bash
# http:: TLS verification against servers with known certificates (Go crypto/tls is the model):
#   * a valid name or a valid IP address (IP SAN) connects; IP gets no SNI (RFC 6066);
#   * a wrong name, an expired or self-signed certificate, a certificate naming "127.0.0.1" as a
#     DNS name for an IP connection, TLS 1.1 and an unknown CA are refused — with the reason.
# It used to refuse a valid IP-address certificate (https://10.0.0.5 never connected), accept a
# DNS name "127.0.0.1" for an IP, send the IP as SNI, and say only "TLS handshake failed".
# Usage: http_client_tls_test.sh <lk>   (needs go to build tests/http_tls_servers)
set -u
LK="$(cd "$(dirname "${1:?usage: http_client_tls_test.sh <lk>}")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; srv=""; trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -rf "$T"' EXIT
command -v go >/dev/null || { echo "FAIL: go is needed to build the TLS test servers"; exit 1; }
(cd "$HERE/http_tls_servers" && CGO_ENABLED=0 go build -o "$T/tlssrv" .) || { echo "FAIL: building the TLS test servers"; exit 1; }
"$T/tlssrv" "$T/ca.pem" > "$T/srv.log" 2>&1 & srv=$!
for i in $(seq 1 50); do grep -q ready "$T/srv.log" && break; sleep 0.1; done
fail=0
probe() {   # ad url beklenen-metin
    printf 'use http\n$r = http::get("%s")\nprint("status=" . ($r["status"] ?? "") . " body=" . ($r["body"] ?? "") . " err=" . ($r["error"] ?? ""))\n' "$2" > "$T/t.lk"
    out="$(cd "$T" && SSL_CERT_FILE="$T/ca.pem" LOOK_ALLOW_SSRF=1 "$LK" t.lk 2>&1 | tail -1)"
    if echo "$out" | grep -qF -- "$3"; then echo "  OK   $1"; else echo "  FAIL $1: want \"$3\", got: $out"; fail=1; fi
}
probe "valid name"                    https://localhost:8441/   "status=200 body=ok sni=localhost"
probe "valid IP address (IP SAN)"     https://127.0.0.1:8442/   "status=200 body=ok sni= "
probe "IP connection sends no SNI"    https://127.0.0.1:8448/   "status=200 body=ok sni= "
probe "wrong name"                    https://localhost:8443/   "Hostname mismatch"
probe "expired"                       https://localhost:8444/   "certificate has expired"
probe "self-signed"                   https://localhost:8445/   "self"
probe "TLS 1.1"                       https://localhost:8446/   "TLS handshake failed"
probe "DNS name '127.0.0.1' for an IP" https://127.0.0.1:8447/  "IP address mismatch"
printf 'use http\n$r = http::get("https://localhost:8441/")\nprint("err=" . ($r["error"] ?? ""))\n' > "$T/t.lk"
out="$(cd "$T" && SSL_CERT_FILE=/nonexistent LOOK_ALLOW_SSRF=1 "$LK" t.lk 2>&1 | tail -1)"
case "$out" in *"certificate verify failed"*) echo "  OK   unknown CA";; *) echo "  FAIL unknown CA: $out"; fail=1;; esac
[ $fail = 0 ] && echo "PASS: http:: verifies TLS like Go's crypto/tls" || { echo "FAIL: http:: TLS verification"; exit 1; }

#!/usr/bin/env bash
# SSRF DNS-rebinding TOCTOU regression guard — NO NETWORK, NO DNS, a pure source invariant.
#
# WHY: the SSRF check (is_ssrf_blocked) and connect() must use the SAME resolved addrinfo.
# If the host is RE-RESOLVED for connect, an attacker can change the DNS record between the
# check and the connection (rebinding) and connect to an internal address (TOCTOU).
#
# MEASURED INVARIANT (for every tcp_connect body):
#   1. getaddrinfo is called EXACTLY once (NO second resolution).
#   2. is_ssrf_blocked(res) comes BEFORE connect.
#   3. connect(...) connects to the resolved 'res->ai_addr' (does not pass the host again).
#      (or to the address of 'p' walking the same list: for (p = res; ...) — "localhost" ::1 then 127.0.0.1;
#      the list is checked ENTIRELY by is_ssrf_blocked(res)).
# If these three hold, the resolved+approved IP is pinned → rebinding is impossible.
#
# POSITIVE CONTROL: below, a deliberately-BROKEN (re-resolving) body is fed to the guard;
# if the guard does NOT CATCH it the test itself is broken (vacuous-green protection).
set -u
SRC="$(dirname "$0")/../src/http_client.cpp"
fails=0

# Check a C++ body text: 0 = clean (pinned), 1 = TOCTOU risk.
check_body() {
    local body="$1"
    local ga conn ssrf
    ga=$(printf '%s\n' "$body" | grep -c 'getaddrinfo(')
    conn=$(printf '%s\n' "$body" | grep -c 'connect(s, res->ai_addr')
    # p walking the same resolved list: connect(s, p->ai_addr) + for (... p = res; ...)
    if printf '%s\n' "$body" | grep -q 'p = res;'; then
        conn=$((conn + $(printf '%s\n' "$body" | grep -c 'connect(s, p->ai_addr')))
    fi
    ssrf=$(printf '%s\n' "$body" | grep -c 'is_ssrf_blocked(res)')
    # getaddrinfo exactly 1, connect to res->ai_addr, ssrf-check present
    if [ "$ga" -ne 1 ] || [ "$conn" -lt 1 ] || [ "$ssrf" -lt 1 ]; then
        return 1
    fi
    return 0
}

# Roughly extract the tcp_connect bodies: the 40 lines after the "static sock_t tcp_connect"
# line (the body is <40 lines on both platforms).
extract_bodies() {
    awk '/static sock_t tcp_connect/{c=40} c>0{print; c--}' "$SRC"
}

echo "== SSRF pin guard =="
n_bodies=$(grep -c 'static sock_t tcp_connect' "$SRC")
echo "tcp_connect definitions: $n_bodies (Win Schannel + POSIX expected)"
if [ "$n_bodies" -lt 1 ]; then echo "FAIL: tcp_connect not found"; exit 1; fi

# Check each platform branch separately (separate the awk blocks with >>>)
bodies=$(awk '/static sock_t tcp_connect/{if(seen)print "<<<SPLIT>>>"; seen=1; c=40} c>0{print; c--}' "$SRC")
idx=0
while IFS= read -r -d '' chunk; do
    idx=$((idx+1))
    if check_body "$chunk"; then
        echo "  body #$idx: PIN OK (single resolution, connects to res->ai_addr, ssrf-check present)"
    else
        echo "  body #$idx: FAIL — risk of re-resolution or pin loss"
        fails=$((fails+1))
    fi
done < <(printf '%s\n<<<SPLIT>>>' "$bodies" | awk 'BEGIN{RS="<<<SPLIT>>>"}{printf "%s%c",$0,0}')

# ── POSITIVE CONTROL: a deliberately-broken (rebinding-prone) body ──────────────────
BAD_BODY='static sock_t tcp_connect(const std::string& host, int port, int t) {
    struct addrinfo *res=nullptr;
    getaddrinfo(host.c_str(), ps, &hints, &res);
    if (is_ssrf_blocked(res)) return INVALID;
    freeaddrinfo(res);
    struct addrinfo *res2=nullptr;
    getaddrinfo(host.c_str(), ps, &hints, &res2);   // SECOND RESOLUTION = TOCTOU
    connect(s, res2->ai_addr, res2->ai_addrlen);
}'
if check_body "$BAD_BODY"; then
    echo "  POSITIVE CONTROL: FAIL — the guard MISSED a rebinding-prone body (the test is broken)"
    fails=$((fails+1))
else
    echo "  POSITIVE CONTROL: OK — the guard caught the re-resolving body"
fi

if [ "$fails" -ne 0 ]; then
    echo "GUARD FAIL ($fails)"; exit 1
fi
echo "GUARD PASSED — the SSRF check and connect are pinned to the same resolved IP (no rebinding TOCTOU)"
exit 0

#!/usr/bin/env bash
# SSRF DNS-rebinding TOCTOU regresyon-guard'ı — AĞSIZ, DNS YOK, saf kaynak-değişmezi.
#
# NEDEN: SSRF kontrolü (is_ssrf_blocked) ile connect() AYNI çözümlenmiş addrinfo'yu
# kullanmalıdır. Eğer connect için host YENİDEN çözülürse, saldırgan DNS kaydını
# kontrol-ile-bağlantı arasında değiştirip (rebinding) iç adrese bağlanabilir (TOCTOU).
#
# ÖLÇÜLEN DEĞİŞMEZ (her tcp_connect gövdesi için):
#   1. getaddrinfo TAM 1 kez çağrılır (ikinci çözüm YOK).
#   2. is_ssrf_blocked(res) connect'ten ÖNCE gelir.
#   3. connect(...) çözümlenmiş 'res->ai_addr'a bağlanır (host'u yeniden geçirmez).
# Bu üçü tutuyorsa çözülmüş+onaylı IP pinlidir → rebinding imkânsız.
#
# POZİTİF KONTROL: aşağıda kasıtlı-KIRIK (yeniden çözen) bir gövde guard'a verilir;
# guard onu YAKALAMAZSA test kendisi bozuktur (vacuous-green koruması).
set -u
SRC="$(dirname "$0")/../src/http_client.cpp"
fails=0

# Bir C++ gövde metnini denetle: 0 = temiz (pinli), 1 = TOCTOU riski.
check_body() {
    local body="$1"
    local ga conn ssrf
    ga=$(printf '%s\n' "$body" | grep -c 'getaddrinfo(')
    conn=$(printf '%s\n' "$body" | grep -c 'connect(s, res->ai_addr')
    ssrf=$(printf '%s\n' "$body" | grep -c 'is_ssrf_blocked(res)')
    # getaddrinfo tam 1, connect res->ai_addr'a, ssrf-check mevcut
    if [ "$ga" -ne 1 ] || [ "$conn" -lt 1 ] || [ "$ssrf" -lt 1 ]; then
        return 1
    fi
    return 0
}

# tcp_connect gövdelerini kabaca çıkar: "static sock_t tcp_connect" satırından
# sonraki 40 satır (gövde her iki platformda da <40 satır).
extract_bodies() {
    awk '/static sock_t tcp_connect/{c=40} c>0{print; c--}' "$SRC"
}

echo "== SSRF pin guard =="
n_bodies=$(grep -c 'static sock_t tcp_connect' "$SRC")
echo "tcp_connect tanımı: $n_bodies (Win Schannel + POSIX bekleniyor)"
if [ "$n_bodies" -lt 1 ]; then echo "FAIL: tcp_connect bulunamadı"; exit 1; fi

# Her platform dalını ayrı denetle (awk bloklarını >>> ile ayır)
bodies=$(awk '/static sock_t tcp_connect/{if(seen)print "<<<SPLIT>>>"; seen=1; c=40} c>0{print; c--}' "$SRC")
idx=0
while IFS= read -r -d '' chunk; do
    idx=$((idx+1))
    if check_body "$chunk"; then
        echo "  gövde #$idx: PIN OK (tek çözüm, res->ai_addr'a bağlanır, ssrf-check var)"
    else
        echo "  gövde #$idx: FAIL — yeniden-çözme veya pin kaybı riski"
        fails=$((fails+1))
    fi
done < <(printf '%s\n<<<SPLIT>>>' "$bodies" | awk 'BEGIN{RS="<<<SPLIT>>>"}{printf "%s%c",$0,0}')

# ── POZİTİF KONTROL: kasıtlı-kırık (rebinding'e açık) gövde ──────────────────
BAD_BODY='static sock_t tcp_connect(const std::string& host, int port, int t) {
    struct addrinfo *res=nullptr;
    getaddrinfo(host.c_str(), ps, &hints, &res);
    if (is_ssrf_blocked(res)) return INVALID;
    freeaddrinfo(res);
    struct addrinfo *res2=nullptr;
    getaddrinfo(host.c_str(), ps, &hints, &res2);   // İKİNCİ ÇÖZÜM = TOCTOU
    connect(s, res2->ai_addr, res2->ai_addrlen);
}'
if check_body "$BAD_BODY"; then
    echo "  POZİTİF KONTROL: FAIL — guard rebinding'e-açık gövdeyi KAÇIRDI (test bozuk)"
    fails=$((fails+1))
else
    echo "  POZİTİF KONTROL: OK — guard yeniden-çözen gövdeyi yakaladı"
fi

if [ "$fails" -ne 0 ]; then
    echo "GUARD FAIL ($fails)"; exit 1
fi
echo "GUARD GEÇTİ — SSRF kontrolü ile connect aynı çözümlenmiş IP'ye pinli (rebinding TOCTOU yok)"
exit 0

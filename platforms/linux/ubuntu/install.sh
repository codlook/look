#!/usr/bin/env bash
# ============================================================================
# LOOK — Ubuntu/Debian tek-komut kurulum
#
#   curl -fsSL https://codlook.com/install/ubuntu.sh | sudo bash
#
# Ne yapar: binary'yi kurar → örnek uygulama + .env → systemd servisi →
# başlatır → doğrular. Geliştirici tek komutla çalışan bir LOOK sunucusu alır.
#
# Ayarlar (env ile geçilebilir):
#   LOOK_PORT=9000            HTTP portu
#   LOOK_WORKERS=0            0 = otomatik (CPU*4, max 64)
#   LOOK_APP_DIR=/var/www/look   uygulama dizini
#   LOOK_BIN_SRC=<yol>        yerel binary klasörü (lk, lk-fcgi) — offline kurulum
#   LOOK_BIN_URL=<url>        binary indirme kök URL'si (release)
# ============================================================================
set -euo pipefail

LOOK_PORT="${LOOK_PORT:-9000}"
LOOK_WORKERS="${LOOK_WORKERS:-0}"
LOOK_APP_DIR="${LOOK_APP_DIR:-/var/www/look}"
LOOK_BIN_URL="${LOOK_BIN_URL:-https://codlook.com/dl/linux}"
BIN_DIR=/usr/local/bin
ETC_DIR=/etc/look

# Self-contained kurulum: script'in yanindaki bin/ klasorunde binary varsa onu kullan
# (release zip'inden cikarilinca 'sudo bash install.sh' dogrudan calisir, indirme yok).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${LOOK_BIN_SRC:-}" ] && [ -f "$SCRIPT_DIR/bin/lk-fcgi" ]; then
  LOOK_BIN_SRC="$SCRIPT_DIR/bin"
fi

c_g="\033[32m"; c_b="\033[34m"; c_y="\033[33m"; c_r="\033[31m"; c_0="\033[0m"
say()  { echo -e "${c_b}▶${c_0} $*"; }
ok()   { echo -e "${c_g}✓${c_0} $*"; }
warn() { echo -e "${c_y}!${c_0} $*"; }
die()  { echo -e "${c_r}✗ $*${c_0}" >&2; exit 1; }

# ── 0. Ön kontroller ────────────────────────────────────────────────────────
[ "$(id -u)" = "0" ] || die "root gerekli — 'sudo' ile çalıştır."
# curl yalnizca indirme (LOOK_BIN_URL) yolunda gerekir; self-contained zip'te bin/
# gomulu oldugundan cogu zaman hic kullanilmaz. Yoksa distro-bagimsiz kur.
if ! command -v curl >/dev/null 2>&1; then
  if   command -v apt-get >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq curl
  elif command -v dnf     >/dev/null 2>&1; then dnf install -y -q curl
  elif command -v yum     >/dev/null 2>&1; then yum install -y -q curl
  fi
fi
ARCH="$(uname -m)"; [ "$ARCH" = "x86_64" ] || warn "test edilen mimari x86_64 (senin: $ARCH)"
. /etc/os-release 2>/dev/null || true
say "Kurulum başlıyor — ${ID:-linux} ${VERSION_ID:-} · port ${LOOK_PORT}"

# ── 1. Binary'yi getir ────────────────────────────────────────────────────────
install_bin() { # $1 = isim
  local name="$1"
  if [ -n "${LOOK_BIN_SRC:-}" ] && [ -f "$LOOK_BIN_SRC/$name" ]; then
    install -m 0755 "$LOOK_BIN_SRC/$name" "$BIN_DIR/$name"
  else
    curl -fsSL "$LOOK_BIN_URL/$name" -o "$BIN_DIR/$name" \
      || die "binary indirilemedi: $LOOK_BIN_URL/$name  (offline için LOOK_BIN_SRC kullan)"
    chmod +x "$BIN_DIR/$name"
  fi
}
say "Binary kuruluyor → $BIN_DIR/{lk, lk-fcgi}"
install_bin lk
install_bin lk-fcgi
ok "lk $("$BIN_DIR/lk" --version 2>/dev/null || echo '(kuruldu)')"

# ── 2. Uygulama + .env ────────────────────────────────────────────────────────
mkdir -p "$LOOK_APP_DIR" "$ETC_DIR"
if [ ! -f "$LOOK_APP_DIR/index.lk" ]; then
  cat > "$LOOK_APP_DIR/index.lk" <<'APP'
# LOOK — örnek uygulama (düzenle: /var/www/look/index.lk)
route("GET", "/", fn() => response::json(["ok" => true, "mesaj" => "Merhaba LOOK!"]))

route("GET", "/selam/{ad}", function($ad) {
    response::json(["selam" => $ad])
})

route("404", fn() => response::error(404, "Bulunamadı"))
APP
  ok "örnek uygulama: $LOOK_APP_DIR/index.lk"
else
  warn "mevcut uygulama korundu: $LOOK_APP_DIR/index.lk"
fi
[ -f "$ETC_DIR/look.env" ] || cat > "$ETC_DIR/look.env" <<ENV
# LOOK ortam değişkenleri
LOOK_PORT=$LOOK_PORT
LOOK_WORKERS=$LOOK_WORKERS
# DB_DSN=mysql://user:pass@127.0.0.1/db
# Gömülü mail sunucusu için (opsiyonel):
# LOOK_SMTP_PORT=25
# LOOK_IMAP_PORT=143
# LOOK_MAIL_DIR=/var/mail/look
ENV
ok "yapılandırma: $ETC_DIR/look.env"

# ── 3. systemd servisi ────────────────────────────────────────────────────────
UNIT=/etc/systemd/system/look.service
cat > "$UNIT" <<UNITEOF
[Unit]
Description=LOOK application server (--mode http)
After=network.target

[Service]
Type=simple
EnvironmentFile=$ETC_DIR/look.env
ExecStart=$BIN_DIR/lk-fcgi --mode http --port \${LOOK_PORT} --workers \${LOOK_WORKERS} $LOOK_APP_DIR/index.lk
Restart=on-failure
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
UNITEOF
ok "systemd unit: $UNIT"

# ── 4. Başlat + doğrula ───────────────────────────────────────────────────────
HAS_SYSTEMD=0
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then HAS_SYSTEMD=1; fi

if [ "$HAS_SYSTEMD" = "1" ]; then
  say "Servis başlatılıyor..."
  systemctl daemon-reload
  systemctl enable --now look.service >/dev/null 2>&1 || systemctl restart look.service
  sleep 2
  if curl -fsS "http://127.0.0.1:${LOOK_PORT}/" >/dev/null 2>&1; then
    ok "servis çalışıyor ve yanıt veriyor"
  else
    warn "servis başladı ama yanıt yok — 'journalctl -u look -n 50' ile bak"
  fi
else
  warn "systemd yok (container?) — servis kurulmadı. Elle çalıştır:"
  echo "    lk-fcgi --mode http --port $LOOK_PORT $LOOK_APP_DIR/index.lk"
fi

# ── 5. Özet ───────────────────────────────────────────────────────────────────
echo
ok "LOOK kuruldu 🎉"
echo -e "   Uygulama : ${c_b}$LOOK_APP_DIR/index.lk${c_0}  (düzenle, servis otomatik yeniden yükler)"
echo -e "   URL      : ${c_b}http://127.0.0.1:${LOOK_PORT}/${c_0}"
[ "$HAS_SYSTEMD" = "1" ] && echo -e "   Servis   : ${c_b}systemctl {status|restart|stop} look${c_0}"
echo -e "   Loglar   : ${c_b}journalctl -u look -f${c_0}"
echo -e "   REPL     : ${c_b}lk repl${c_0}   ·   Çalıştır: ${c_b}lk dosya.lk${c_0}"

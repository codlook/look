#!/usr/bin/env bash
# ============================================================================
# LOOK — Ubuntu/Debian tek-komut kurulum
#
#   1) look-lang-linux.zip indir (GitHub Releases):
#      https://github.com/codlook/look/releases/latest/download/look-lang-linux.zip
#   2) unzip look-lang-linux.zip -d look-lang && cd look-lang   (zip'te üst klasör yok)
#   3) sudo bash install.sh      # bin/ zip'in içinde gömülü — indirme yok
#
# Ne yapar: binary'yi kurar → örnek uygulama + .env → systemd servisi →
# başlatır → doğrular. Geliştirici tek komutla çalışan bir LOOK sunucusu alır.
#
# Ayarlar (env ile geçilebilir):
#   LOOK_PORT=9000            HTTP portu
#   LOOK_WORKERS=0            0 = otomatik (CPU*4, max 64)
#   LOOK_APP_DIR=/var/www/look   uygulama dizini
#   LOOK_BIN_SRC=<yol>        yerel binary klasörü (lk, lk-fcgi) — offline kurulum
#   LOOK_BIN_URL=<url>        opsiyonel: loose binary (lk, lk-fcgi) sunan bir ayna kök URL'si
#                             (normalde gerekmez — release zip'i bin/ ile gelir)
# ============================================================================
set -euo pipefail

# Seçenek yok; ayarlar ortam değişkeniyle. Eskiden verilen her seçenek sessizce yok sayılıyordu
# (belge --with-nginx diyordu: kullanıcı nginx kuruldu sandı, port dışarıya açık kaldı).
# Bilinmeyen seçenek → hiçbir şey kurmadan, nedenini söyleyerek dur.
usage() {
  cat <<'USAGE'
Usage: sudo bash install.sh

Installs lk, lk-fcgi and lk-cgi to /usr/local/bin, a sample app, /etc/look/look.env and a
systemd service (look.service, user look) that runs lk-fcgi --mode http. It does not install or
configure nginx or Apache.

Settings are environment variables (sudo LOOK_PORT=9100 bash install.sh):
  LOOK_PORT=9000           HTTP port
  LOOK_WORKERS=0           worker threads (0 = automatic)
  LOOK_APP_DIR=/var/www/look   app directory
  LOOK_BIN_SRC=<dir>       local folder with the binaries (default: bin/ next to this script)

The service listens on every interface. Behind nginx or Apache, add LOOK_BIND=127.0.0.1 to
/etc/look/look.env and run: systemctl restart look
USAGE
}
for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
    *) echo "install.sh: unknown option: $arg (install.sh takes no options; settings are environment variables, see --help)" >&2; exit 2 ;;
  esac
done

LOOK_PORT="${LOOK_PORT:-9000}"
LOOK_WORKERS="${LOOK_WORKERS:-0}"
LOOK_APP_DIR="${LOOK_APP_DIR:-/var/www/look}"
LOOK_BIN_URL="${LOOK_BIN_URL:-}"
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
  # curl KURULUMU opsiyonel/kolaylık — self-contained zip'te (bin/ gömülü) hiç kullanılmaz.
  # set -e altında ağ yoksa 'apt-get install' patlar ve KRİTİK binary kurulumundan ÖNCE tüm
  # script'i düşürür (XAMPP install.ps1 bug'ının kardeşi). '|| true' ile best-effort yap;
  # curl gerçekten gerekiyorsa (indirme yolu) install_bin'in kendi 'die'ı net hata verir.
  if   command -v apt-get >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq curl || true
  elif command -v dnf     >/dev/null 2>&1; then dnf install -y -q curl || true
  elif command -v yum     >/dev/null 2>&1; then yum install -y -q curl || true
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
  elif [ -n "${LOOK_BIN_URL:-}" ]; then
    curl -fsSL "$LOOK_BIN_URL/$name" -o "$BIN_DIR/$name" \
      || die "binary indirilemedi: $LOOK_BIN_URL/$name"
    chmod +x "$BIN_DIR/$name"
  else
    die "binary bulunamadı — install.sh'i açılmış release zip'inin içinden çalıştır (bin/ yanında olmalı), ya da LOOK_BIN_URL/LOOK_BIN_SRC ayarla"
  fi
}
say "Binary kuruluyor → $BIN_DIR/{lk, lk-fcgi}"
install_bin lk
install_bin lk-fcgi
# lk-cgi (Apache CGI için) 1.0.41'den beri pakette; elle verilen eski bir klasörde yoksa atlanır.
if [ -n "${LOOK_BIN_SRC:-}" ] && [ -f "$LOOK_BIN_SRC/lk-cgi" ]; then
  install -m 0755 "$LOOK_BIN_SRC/lk-cgi" "$BIN_DIR/lk-cgi"
  ok "lk-cgi → $BIN_DIR/lk-cgi"
fi
ok "lk $("$BIN_DIR/lk" --version 2>/dev/null || echo '(kuruldu)')"

# ── 2. Uygulama + .env ────────────────────────────────────────────────────────
mkdir -p "$LOOK_APP_DIR" "$ETC_DIR"
if [ ! -f "$LOOK_APP_DIR/index.lk" ]; then
  cat > "$LOOK_APP_DIR/index.lk" <<'APP'
# LOOK — örnek uygulama (düzenle: /var/www/look/index.lk)
route("GET", "/", fn() => response::json(["ok" => true, "message" => "Hello from LOOK!"]))

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
# Behind nginx or Apache on this machine, listen on 127.0.0.1 only:
# LOOK_BIND=127.0.0.1
# DB_DSN=mysql://user:pass@127.0.0.1/db
# Gömülü mail sunucusu için (opsiyonel):
# LOOK_SMTP_PORT=25
# LOOK_IMAP_PORT=143
# LOOK_MAIL_DIR=/var/mail/look
ENV
ok "yapılandırma: $ETC_DIR/look.env"

# ── 3. Servis kullanıcısı ─────────────────────────────────────────────────────
# Servis root olarak çalışıyordu (User= yoktu): uygulamadaki bir açık bütün sunucuyu verirdi.
# Artık kendi sistem kullanıcısı: uygulama dizini ona ait (SQLite, yüklenen dosyalar yazılabilsin).
if ! id look >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/look --no-create-home --shell /usr/sbin/nologin look \
    || die "could not create the system user 'look'"
  ok "system user: look"
fi
chown -R look:look "$LOOK_APP_DIR"

# ── 4. systemd servisi ────────────────────────────────────────────────────────
UNIT=/etc/systemd/system/look.service
cat > "$UNIT" <<UNITEOF
[Unit]
Description=LOOK application server (--mode http)
After=network.target

[Service]
Type=simple
User=look
Group=look
EnvironmentFile=$ETC_DIR/look.env
# Oturum dosyaları kalıcı, yalnız look kullanıcısına ait 0700 dizinde (yeniden başlatmada düşmez);
# /tmp servise özel. Düşük portlar (80, 25) root olmadan da açılabilsin.
StateDirectory=look
StateDirectoryMode=0700
Environment=LOOK_SESSION_DIR=/var/lib/look/sessions
PrivateTmp=true
NoNewPrivileges=true
AmbientCapabilities=CAP_NET_BIND_SERVICE
ExecStart=$BIN_DIR/lk-fcgi --mode http --port \${LOOK_PORT} --workers \${LOOK_WORKERS} $LOOK_APP_DIR/index.lk
Restart=on-failure
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
UNITEOF
ok "systemd unit: $UNIT"

# ── 5. Başlat + doğrula ───────────────────────────────────────────────────────
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

# ── 6. Özet ───────────────────────────────────────────────────────────────────
echo
ok "LOOK kuruldu 🎉"
echo -e "   Uygulama : ${c_b}$LOOK_APP_DIR/index.lk${c_0}  (düzenle, servis otomatik yeniden yükler)"
echo -e "   URL      : ${c_b}http://127.0.0.1:${LOOK_PORT}/${c_0}"
[ "$HAS_SYSTEMD" = "1" ] && echo -e "   Servis   : ${c_b}systemctl {status|restart|stop} look${c_0}"
echo -e "   Loglar   : ${c_b}journalctl -u look -f${c_0}"
echo -e "   REPL     : ${c_b}lk repl${c_0}   ·   Çalıştır: ${c_b}lk dosya.lk${c_0}"

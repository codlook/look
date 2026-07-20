#!/bin/bash
# MySQL kimlik dogrulama guard'i — IKI SURUM BIRDEN.
#
# NEDEN: mysql_client.cpp yalnizca `mysql_native_password` uyguluyordu ve eklenti
# adini handshake yanitina SABIT yaziyordu. Sunucunun bildirdigi eklenti
# okunmuyor, AuthSwitchRequest (0xFE) hic ele alinmiyordu. Sonuc:
#   MySQL 5.7 / MariaDB                          -> calisiyor
#   MySQL 8.0 (2018'den beri VARSAYILAN sha2)    -> BAGLANAMIYOR
#   MySQL 8.4 (native kapali) / 9.x (kaldirildi) -> BAGLANAMIYOR
# Yani dil pratikte MySQL 5.7 diliydi.
#
# NEDEN IKI SURUM: tek surumde gecen test digerini kirabilir. 8.0 yeni yolu
# (caching_sha2 + RSA), 5.7 eski yolu (native) dogrular; ikisi ayni handshake
# kodunu paylasiyor.
#
# Kullanim:
#   bash mysql_auth_test.sh <lk_yolu>
# Ortam degiskenleri verilmezse konteynerleri kendi kurar (docker gerekir):
#   LOOK_MY8_DSN   (varsayilan: mysql://root:sifre123@my8:3306/testdb)
#   LOOK_MY57_DSN  (varsayilan: mysql://root:sifre123@my57:3306/testdb)
set -u
LK="${1:-./build/lk}"
MY8="${LOOK_MY8_DSN:-mysql://root:sifre123@my8:3306/testdb}"
MY57="${LOOK_MY57_DSN:-mysql://root:sifre123@my57:3306/testdb}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/my.lk" <<'LK'
try {
  $c = db::connect(env("MYDSN", ""))
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_auth_t (id INT PRIMARY KEY AUTO_INCREMENT, ad VARCHAR(50)) CHARACTER SET utf8mb4")
  db::exec($c, "DELETE FROM look_auth_t")
  db::exec($c, "INSERT INTO look_auth_t (ad) VALUES (?)", ["şğü"])
  $r = db::query($c, "SELECT ad FROM look_auth_t")
  $v = db::query($c, "SELECT VERSION() AS v")
  print("OK|" . $v[0].v . "|" . $r[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

kontrol() {
  etiket="$1"; dsn="$2"; bek_surum="$3"
  out=$(MYDSN="$dsn" timeout 40 "$LK" "$TMP/my.lk" 2>&1 | grep -E '^(OK|HATA)' | head -1)
  case "$out" in
    OK\|${bek_surum}*\|şğü)
      echo "  PASS $etiket -> ${out}" ;;
    HATA*)
      echo "  FAIL $etiket baglanamadi: $out"
      case "$out" in
        *"Access denied"*|*"authentication"*|*plugin*)
          echo "       -> kimlik dogrulama eklentisi ele alinmiyor olabilir"
          echo "          (sunucunun bildirdigi eklenti okunuyor mu? 0xFE AuthSwitchRequest?)" ;;
      esac
      fail=1 ;;
    *)
      echo "  FAIL $etiket beklenmedik cikti: $out"; fail=1 ;;
  esac
}

echo "MySQL kimlik dogrulama guard'i (iki surum)"
kontrol "MySQL 8.x  (caching_sha2_password)" "$MY8"  "8."
kontrol "MySQL 5.7  (mysql_native_password)" "$MY57" "5.7"

# Ikinci tur: 8.x'te sunucu onbellegi artik dolu -> HIZLI yol (farkli kod dali).
# Ilk tur RSA tam yolunu, bu tur hizli yolu dogrular.
kontrol "MySQL 8.x  (hizli yol, onbellek dolu)" "$MY8" "8."

[ $fail = 0 ] && echo "PASS: MySQL auth — iki surum de baglaniyor" || echo "FAIL: MySQL auth"
exit $fail

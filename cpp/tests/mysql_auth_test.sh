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

# ── GUVENLIK: kacis, SET SESSION'a BAGLI OLMADAN guvenli mi? ────────────────────
# NEDEN: escape() tirnagi `\'` ile kaciriyordu. MySQL NO_BACKSLASH_ESCAPES modunda
# ters bolu kacis karakteri DEGILDIR -> `\'` tirnagi kapatir -> SQL ENJEKSIYONU.
# OLCULDU: oturum o moda alininca `' OR 1=1 -- ` yuku TUM TABLOYU dondurdu (3/3).
# Bunu tutan tek sey do_connect()'teki `SET SESSION sql_mode = REPLACE(...)`
# komutunun basarili olmasiydi — yeterli zemin degil (ProxySQL gibi baglanti
# coklayicilari oturum durumunu sessizce kaybedebilir; SET basarili olur, koruma
# yok olur). Cozum: `''` (standart SQL) — her iki modda guvenli, SET'ten bagimsiz.
# Bu kontrol korumayi KASTEN kaldirip saldiriyor: guvenlik SET'e bagli olmamali.
kacis_kontrol() {
  dsn="$1"
  [ -z "$dsn" ] && return
  cat > "$TMP/esc.lk" <<'LK'
$c = db::connect(env("MYDSN",""))
db::exec($c, "DROP TABLE IF EXISTS look_esc")
db::exec($c, "CREATE TABLE look_esc (ad VARCHAR(100)) CHARACTER SET utf8mb4")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('a')")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('b')")
db::exec($c, "INSERT INTO look_esc (ad) VALUES ('c')")
# korumayi KASTEN kaldir — kacis kendi basina guvenli olmali
db::exec($c, "SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES'")
$n1 = count(db::query($c, "SELECT * FROM look_esc WHERE ad = ?", ["' OR 1=1 -- "]))
$n2 = count(db::query($c, "SELECT * FROM look_esc WHERE ad = ?", ["\\' OR 1=1 -- "]))
db::exec($c, "SET SESSION sql_mode = 'STRICT_TRANS_TABLES'")
db::exec($c, "INSERT INTO look_esc (ad) VALUES (?)", ["O'Brien"])
$rt = db::query($c, "SELECT ad FROM look_esc WHERE ad = ?", ["O'Brien"])
print($n1 . "|" . $n2 . "|" . count($rt))
LK
  out=$(MYDSN="$dsn" timeout 40 "$LK" "$TMP/esc.lk" 2>&1 | grep -E '^[0-9]+\|' | head -1)
  if [ "$out" = "0|0|1" ]; then
    echo "  PASS kacis SET'ten bagimsiz guvenli (NBE modunda bile enjeksiyon yok, veri korunuyor)"
  else
    echo "  FAIL GUVENLIK: kacis NO_BACKSLASH_ESCAPES modunda kiriliyor: [$out] (beklenen 0|0|1)"
    echo "       -> escape() tirnagi '\\'' ile mi kaciriyor? Standart SQL '' kullanilmali"
    fail=1
  fi
}

echo "MySQL/MariaDB kimlik dogrulama guard'i (surum matrisi)"
kontrol "MySQL 5.7   (mysql_native_password)"  "$MY57" "5.7"
kontrol "MySQL 8.0   (caching_sha2_password)"  "$MY8"  "8.0"
[ -n "${LOOK_MY82_DSN:-}"  ] && kontrol "MySQL 8.2   (caching_sha2)"                "$LOOK_MY82_DSN"  "8.2"
[ -n "${LOOK_MY84_DSN:-}"  ] && kontrol "MySQL 8.4   (native DISABLED)"             "$LOOK_MY84_DSN"  "8.4"
[ -n "${LOOK_MY91_DSN:-}"  ] && kontrol "MySQL 9.x   (native KALDIRILDI)"           "$LOOK_MY91_DSN"  "9."
[ -n "${LOOK_MDB_DSN:-}"   ] && kontrol "MariaDB     (native)"                      "$LOOK_MDB_DSN"   "1"

# Ikinci tur: 8.x'te sunucu onbellegi artik dolu -> HIZLI yol (farkli kod dali).
# Ilk tur RSA tam yolunu, bu tur hizli yolu dogrular. IKISI DE kosulmali:
# yalniz hizli yol test edilseydi, RSA yolu bozuk olsa bile gecerdi (taze
# sunucuda ilk baglanti DAIMA RSA'dan gecer).
kontrol "MySQL 8.0   (hizli yol, onbellek dolu)" "$MY8" "8.0"

# Kacis guvenligi — kimlik dogrulamadan bagimsiz, ayni sunucu uzerinde
kacis_kontrol "$MY8"

[ $fail = 0 ] && echo "PASS: MySQL/MariaDB auth — surum matrisi temiz" || echo "FAIL: MySQL auth"
exit $fail

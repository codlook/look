#!/bin/bash
# DB SURUM-FARKINDALIK guard'i — MySQL / MariaDB / PostgreSQL / SQLite.
#
# AMAC: "yeni DB surumu cikti ve LOOK'u kirdi" durumunu KULLANICI sikayet etmeden,
# surumun ciktigi gun yakalamak. Bugun MySQL 8'in caching_sha2_password'e gecmesi
# tam bu sinifta bir surprizdi (39. bug) — dil sessizce baglanamaz olmustu.
#
# Nasil calisir: her DB'nin `latest` etiketli konteynerine baglanip yaz/oku +
# surum sorgusu yapar. CI'da haftalik kosarsa, upstream bir kirilma getirdiginde
# FAIL kirmizisi cikar. `latest`, bugunku pin'li matristen (mysql_auth_test.sh)
# FARKLIDIR: o test edilen surumleri kilitler, bu gelecegi izler.
#
# Kullanim:
#   bash db_latest_test.sh <lk_yolu>
# DSN'ler ortamdan (verilmeyen DB atlanir, 'atlandi' yazarak — sessizce degil):
#   LOOK_MYSQL_LATEST_DSN   mysql://root:pw@host:3306/db
#   LOOK_MARIADB_LATEST_DSN mysql://root:pw@host:3306/db
#   LOOK_PG_LATEST_DSN      postgres://user:pw@host:5432/db
#   (SQLite gomulu — DSN gerektirmez, her zaman kosar)
set -u
LK="${1:-./build/lk}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# MySQL/MariaDB (ayni wire) — utf8mb4, gidis-donuste Turkce korunmali
cat > "$TMP/my.lk" <<'LK'
try {
  $c = db::connect(env("D",""))
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_lt (id INT PRIMARY KEY AUTO_INCREMENT, ad VARCHAR(50)) CHARACTER SET utf8mb4")
  db::exec($c, "DELETE FROM look_lt")
  db::exec($c, "INSERT INTO look_lt (ad) VALUES (?)", ["şğü"])
  print("OK|" . db::query($c,"SELECT VERSION() AS v")[0].v . "|" . db::query($c,"SELECT ad FROM look_lt")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

# PostgreSQL — SERIAL/TEXT + sequence'SIZ tablo transaction (39... lastval veri kaybi)
# NEDEN: PG'de INSERT sonrasi otomatik SELECT lastval() cagriliyor; tablo SERIAL
# kullanmiyorsa lastval hata verir ve ACIK TRANSACTION'i abort eder -> COMMIT
# sessizce ROLLBACK olur -> veri kaybolur. Bu kontrol sequence'SIZ bir tabloya
# transaction icinde INSERT + COMMIT yapip verinin KALDIGINI dogrular.
cat > "$TMP/pg.lk" <<'LK'
use string
try {
  $c = db::connect(env("D",""))
  # sequence'SIZ tablo + transaction — lastval veri kaybi tuzagi.
  # DIKKAT: bu test SERIAL insert'ten ONCE olmali. Aksi halde SERIAL insert
  # session'da lastval'i TANIMLAR ve sequence'siz insert'te lastval artik hata
  # vermez -> bug tetiklenmez (guard yaniltici PASS verir — bu tam olarak yasandi).
  db::exec($c, "DROP TABLE IF EXISTS look_notx")
  db::exec($c, "CREATE TABLE look_notx (n INT)")
  db::begin($c)
  db::exec($c, "INSERT INTO look_notx (n) VALUES (1)")
  db::commit($c)
  $kaldi = count(db::query($c, "SELECT n FROM look_notx"))
  if ($kaldi != 1) { print("HATA|sequence'siz tabloda tx-ici INSERT kayboldu (lastval abort): " . $kaldi . " satir"); return }
  # SERIAL testi (last_id + version)
  db::exec($c, "CREATE TABLE IF NOT EXISTS look_lt (id SERIAL PRIMARY KEY, ad TEXT)")
  db::exec($c, "DELETE FROM look_lt")
  db::exec($c, "INSERT INTO look_lt (ad) VALUES (?)", ["şğü"])
  print("OK|" . string::substr(db::query($c,"SELECT version() AS v")[0].v, 0, 20) . "|" . db::query($c,"SELECT ad FROM look_lt")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

# SQLite — gomulu, DSN yok
cat > "$TMP/sq.lk" <<'LK'
use string
try {
  $c = db::connect("sqlite://:memory:")
  db::exec($c, "CREATE TABLE t (id INTEGER PRIMARY KEY, ad TEXT)")
  db::exec($c, "INSERT INTO t (ad) VALUES (?)", ["şğü"])
  print("OK|" . db::query($c,"SELECT sqlite_version() AS v")[0].v . "|" . db::query($c,"SELECT ad FROM t")[0].ad)
} catch ($e) { print("HATA|" . $e) }
LK

kontrol() {
  etiket="$1"; script="$2"; dsn="${3:-}"
  if [ "$script" != "$TMP/sq.lk" ] && [ -z "$dsn" ]; then
    echo "  ATLANDI $etiket (DSN verilmedi)"; return
  fi
  out=$(D="$dsn" timeout 45 "$LK" "$script" 2>&1 | grep -E '^(OK|HATA)' | head -1)
  case "$out" in
    OK\|*\|şğü) echo "  PASS $etiket -> ${out#OK|}" ;;
    *)          echo "  FAIL $etiket -> $out"; fail=1 ;;
  esac
}

echo "DB latest surum-farkindalik guard'i"
kontrol "MySQL   latest" "$TMP/my.lk" "${LOOK_MYSQL_LATEST_DSN:-}"
kontrol "MariaDB latest" "$TMP/my.lk" "${LOOK_MARIADB_LATEST_DSN:-}"
kontrol "Postgres latest" "$TMP/pg.lk" "${LOOK_PG_LATEST_DSN:-}"
kontrol "SQLite  (gomulu)" "$TMP/sq.lk"

[ $fail = 0 ] && echo "PASS: DB latest — baglanabilen tum surumler temiz" || echo "FAIL: DB latest"
exit $fail

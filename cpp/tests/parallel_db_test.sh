#!/bin/bash
# parallel() + DB regresyon testi — bağlantı sızıntısı ve havuz baskısı.
#
# Sızıntı sınıfı sinsidir: ilk 1-2 istek ÇALIŞIR, sonra havuz tükenir ve endpoint
# sonsuza dek asılır (log'da hata YOK). Bu yüzden tek istekle test etmek yetmez —
# havuz boyutundan FAZLA istek atmak şart.
#
# Kök neden (fix'lendi): parallel() ham std::thread yaratır; task içindeki db::query,
# get_conn üzerinden bu thread'in thread_local'ine TEMBEL bir conn alır (istek thread'i
# gibi acquire_thread_connections() çağrılmaz). Task bitince release edilmezse conn
# havuza DÖNMEZ → kalıcı sızıntı. Fix: task lambda'sında RAII DbGuard →
# release_thread_connections() (hem VM/PARALLEL_CALL hem interpreter parallel()).
#
# Kullanım: bash parallel_db_test.sh [lk-fcgi_yolu]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
FCGI="${1:-$DIR/../build/lk-fcgi}"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"   # mutlak: $TMP icine cd edecegiz
TMP=$(mktemp -d)
PORT=9671
trap 'rm -rf "$TMP"' EXIT

# DB yolu GÖRELİ + sunucu $TMP içinde çalışır: mutlak MSYS yolu ("/tmp/...") Windows
# binary'sinde açılamıyor (Linux'ta sorunsuz). Göreli yol iki platformda da çalışır.
cat > "$TMP/app.lk" <<'LK'
$db = db::connect("sqlite://t.db")
db::exec($db, "CREATE TABLE IF NOT EXISTS t (v TEXT)")
db::exec($db, "DELETE FROM t")
db::exec($db, "INSERT INTO t VALUES ('x')")
route("GET","/par", function(){
  $ch = channel(1)
  parallel(function(){ send($ch, count(db::query($db, "SELECT v FROM t"))) })
  return response::text("par=" . receive($ch))
})
route("GET","/par8", function(){
  $ch = channel(8)
  $i = 0
  while ($i < 8) { parallel(function(){ send($ch, count(db::query($db, "SELECT v FROM t"))) }); $i = $i + 1 }
  $sum = 0; $j = 0
  while ($j < 8) { $sum = $sum + receive($ch); $j = $j + 1 }
  return response::text("sum=" . $sum)
})
LK

cd "$TMP"   # göreli "t.db" buraya düşsün; $srv gerçek fcgi PID'i olsun (alt-kabuk değil)

fail=0
for engine in vm interp; do
  rm -f t.db
  if [ "$engine" = "interp" ]; then export LOOK_BYTECODE=0; else unset LOOK_BYTECODE; fi
  "$FCGI" --mode http --port $PORT app.lk >/dev/null 2>&1 &
  srv=$!
  sleep 2

  # 1) Sızıntı: havuz boyutundan (varsayılan 4) fazla ardışık istek
  ok=0
  for i in $(seq 1 10); do
    [ "$(curl -s --max-time 6 "http://127.0.0.1:$PORT/par")" = "par=1" ] && ok=$((ok+1))
  done
  # 2) Havuz baskısı: 8 eşzamanlı task / 4 conn
  sum=$(curl -s --max-time 18 "http://127.0.0.1:$PORT/par8")

  kill $srv 2>/dev/null || true
  wait $srv 2>/dev/null || true
  sleep 1

  if [ "$ok" = "10" ] && [ "$sum" = "sum=8" ]; then
    echo "PASS [$engine]: sızıntı yok (10/10), havuz baskısı OK ($sum)"
  else
    echo "FAIL [$engine]: ardışık=$ok/10 (sızıntı!), 8-paralel=[$sum] (beklenen sum=8)"
    fail=1
  fi
done
unset LOOK_BYTECODE
exit $fail

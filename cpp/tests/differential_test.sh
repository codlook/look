#!/bin/bash
# ÜÇ MOTOR differential regresyon testi — hepsi BİREBİR aynı çıktıyı vermeli.
# Felsefe koruması: motorlar SEMANTİK olarak ayrışmamalı ("bagsız ve stabil").
#
#   1) tree-walk  : LOOK_CLI_VM=0 lk <script>   — referans semantik (interpreter, kacis kapagi)
#   2) CLI-VM     : lk <script>  (C9: DEFAULT) — bytecode VM, main.cpp/build_cli_builtins
#   3) web-VM     : lk-fcgi --mode http (route)  — bytecode VM, http_main.cpp/req_builtins
#
# NEDEN 3 MOTOR: builtin wiring İKİ ayrı elle-yazılmış yerde duruyor (build_cli_builtins
# ve req_builtins). Bunlar sessizce birbirinden kayabilir — nitekim kaydı da (math::round
# bir dönem web'de çalışıp CLI-VM'de çalışmadı). 2 motorlu test bu kaymayı GÖREMEZ.
#
# Her motor değişikliğinden (opcode/builtin/Value/köprü) sonra çalıştır.
# Kullanım: bash differential_test.sh [lk_yolu] [lk-fcgi_yolu]
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
LK="${1:-$DIR/../build/lk}"
FCGI="${2:-$DIR/../build/lk-fcgi}"
BODY="$DIR/differential_body.lk"
PORT=9611
TMP=$(mktemp -d)
SRV=""
# Temizlik PID ile — GENİŞ "pkill -f" ASLA (paylaşımlı/prod sunucuda baska sürecleri
# öldürür; bir kez prod kesintisine yol açtı).
cleanup() {
  [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
  sleep 1
  [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

cp "$BODY" "$TMP/cli.lk"; echo 'print(run_all());' >> "$TMP/cli.lk"
cp "$BODY" "$TMP/vm.lk";  echo 'route("GET","/t", function(){ return response::text(run_all()); });' >> "$TMP/vm.lk"

# C9: `lk` artık DEFAULT olarak VM kullanır → tree-walk için LOOK_CLI_VM=0 gerekir.
TREE=$(LOOK_CLI_VM=0 "$LK" "$TMP/cli.lk" 2>&1)
CLIVM=$("$LK" "$TMP/cli.lk" 2>&1)

"$FCGI" --mode http --port $PORT "$TMP/vm.lk" >/dev/null 2>&1 &
SRV=$!
sleep 2
WEBVM=$(curl -s --max-time 15 "http://127.0.0.1:$PORT/t")

fail=0
[ -z "$TREE" ] && { echo "FAIL: tree-walk bos cikti verdi (script hatasi?)"; fail=1; }
[ "$TREE" = "$CLIVM" ] || { echo "FAIL: tree-walk != CLI-VM"; fail=1; }
[ "$TREE" = "$WEBVM" ] || { echo "FAIL: tree-walk != web-VM"; fail=1; }

# ── CLI'ye özgü yüzey: print/write BAYT BAYT + exit kodu ─────────────────────────
# Yukarıdaki $(...) karşılaştırması trailing newline'ı YUTAR ve gövde tek kez print
# eder → print'in newline/" " ayraç semantiği görünmez. Nitekim VM'de print newline de
# ayraç da eklemiyordu (interpreter: args " " ile + "\n") ve bu guard'dan KAÇMIŞTI.
cat > "$TMP/io.lk" <<'LK'
print("A")
write("B")
print("C")
print("x", "y", "z")
write("p", "q")
print("son")
LK
LOOK_CLI_VM=0 "$LK" "$TMP/io.lk" > "$TMP/io_tree.out" 2>&1
"$LK" "$TMP/io.lk" > "$TMP/io_vm.out" 2>&1
# Bayt-bayt karsilastirma HARICI ARACA BAGLI OLMAMALI: `cmp`/`diff` (diffutils) minimal
# konteynerlerde YOK — komut bulunamayinca non-zero doner ve "baytlar farkli" sanilir
# (YANLIS ALARM: bu tam olarak yasandi, AlmaLinux 8 minimal'da cmp yok). od + $(...)
# de guvenli degil: $(...) trailing newline'i yutar. Cozum: od dump'larini DOSYAYA yaz,
# shell string karsilastirmasi yap — od her yerde var (coreutils).
od -c < "$TMP/io_tree.out" > "$TMP/io_tree.od"
od -c < "$TMP/io_vm.out"   > "$TMP/io_vm.od"
io_tree_dump=$(cat "$TMP/io_tree.od")
io_vm_dump=$(cat "$TMP/io_vm.od")
if [ "$io_tree_dump" != "$io_vm_dump" ]; then
  echo "FAIL: print/write BAYT BAYT ayrisiyor (newline / ' ' ayraci)"
  echo "  tree : $(echo "$io_tree_dump" | head -2 | tr '\n' ' ')"
  echo "  CLIVM: $(echo "$io_vm_dump"   | head -2 | tr '\n' ' ')"
  fail=1
fi
printf 'print("basla")\nexit(3)\nprint("OLMAMALI")\n' > "$TMP/ex.lk"
LOOK_CLI_VM=0 "$LK" "$TMP/ex.lk" >/dev/null 2>&1; t_code=$?
"$LK" "$TMP/ex.lk" >/dev/null 2>&1; v_code=$?
if [ "$t_code" != "3" ] || [ "$v_code" != "3" ]; then
  echo "FAIL: exit(3) kodu — tree=$t_code CLI-VM=$v_code (ikisi de 3 olmali)"
  fail=1
fi


# ── VM fallback GÜRÜLTÜLÜ mü? (C9 felsefe adımı) ─────────────────────────────
# Fallback bug MASKELER: route sessizce yavaş yola düşüp doğru sonuç döner → bug
# yıllarca görünmez (2026-07-16'da bulunan 10 bug'ın çoğu böyle saklanmıştı).
# Sözleşme: (1) fallback olursa log seviyesi ERROR + "VM BUG" ibaresi,
#           (2) LOOK_VM_STRICT=1 → maskeleme YOK, hata yüzeye çıkar (CI/staging).
cat > "$TMP/fb.lk" <<'LK'
use jobs
route("GET","/w",  function(){ jobs::worker("q", function($j){ return 1 }); return response::text("ok") })
route("GET","/ok", function(){ return response::text("saglam") })
LK
FB_PORT=9612
"$FCGI" --mode http --port $FB_PORT "$TMP/fb.lk" >"$TMP/fb1.log" 2>&1 &
FB=$!
sleep 2
w_def=$(curl -s --max-time 8 "http://127.0.0.1:$FB_PORT/w")
kill $FB 2>/dev/null; sleep 1; kill -9 $FB 2>/dev/null
grep -q "VM BUG" "$TMP/fb1.log" || { echo "FAIL: fallback SESSİZ (ERROR+'VM BUG' logu yok) — bug maskelenir"; fail=1; }
[ "$w_def" = "ok" ] || { echo "FAIL: default fallback calismiyor (guvenlik agi bozuk): [$w_def]"; fail=1; }

LOOK_VM_STRICT=1 "$FCGI" --mode http --port $FB_PORT "$TMP/fb.lk" >"$TMP/fb2.log" 2>&1 &
FB=$!
sleep 2
w_str=$(curl -s --max-time 8 "http://127.0.0.1:$FB_PORT/w")
ok_str=$(curl -s --max-time 8 "http://127.0.0.1:$FB_PORT/ok")
kill $FB 2>/dev/null; sleep 1; kill -9 $FB 2>/dev/null
[ "$w_str" = "ok" ] && { echo "FAIL: LOOK_VM_STRICT=1 hala MASKELIYOR (fallback calisti)"; fail=1; }
[ "$ok_str" = "saglam" ] || { echo "FAIL: strict modda saglam route bozuldu: [$ok_str]"; fail=1; }

# ── Dispatch kopyasi worker'da YENIDEN KULLANILIYOR mu, izolasyon bozuldu mu? ──
# Kopya artik thread_local (per-request make_dispatch_copy 30-63us idi — route'u
# calistirmaktan pahali). Sozlesme: paylasilan kopya istekler arasi SIZINTI yapmamali.
#   $sayac = $sayac + 1  → fonksiyon-local (her istek 1)  — atama izole
#   push($liste, ...)    → yakalanan diziyi paylasir (LOOK modeli: $db gibi) — DEGISMEMELI
# Bu degerler thread_local optimizasyonu ONCESI ile BIREBIR ayni olmali.
cat > "$TMP/iso.lk" <<'LK'
$sayac = 0
route("GET","/inc", function(){ $sayac = $sayac + 1; return response::text("s=" . $sayac) })
LK
ISO_PORT=9613
"$FCGI" --mode http --port $ISO_PORT "$TMP/iso.lk" >/dev/null 2>&1 &
IS=$!
sleep 2
iso_all=""
for i in 1 2 3 4 5; do iso_all="$iso_all$(curl -s --max-time 8 "http://127.0.0.1:$ISO_PORT/inc")"; done
kill $IS 2>/dev/null; sleep 1; kill -9 $IS 2>/dev/null
[ "$iso_all" = "s=1s=1s=1s=1s=1" ] || { echo "FAIL: dispatch kopyasi izolasyonu BOZUK (global sizinti): [$iso_all] (beklenen s=1 x5)"; fail=1; }

# ── ODR: istemci/sunucu tip cakismasi (LTO ile CRASH) ────────────────────────
# look::HttpResponse HEM http_client.h HEM http_server.h'de FARKLI tanimliydi (ayni
# namespace!). look-fcgi ikisini de linkler → TANIMSIZ DAVRANIS; LTO acikken iki
# layout tek tip sanilip birlestiriliyor → nesne bir duzenle kurulup digeriyle
# yikiliyor → "free(): invalid pointer"/segfault. Web route'undan http::get cagirmak
# sunucuyu COKERTIYORDU (tek istek yetiyordu). CLI'de yoktu (http_server.cpp linkli
# degil), ASan'da yoktu (o build'de LTO kapali) — bu yuzden aylarca gorunmedi.
# Sozlesme: http::get web route'unda calismali ve sunucu AYAKTA kalmali.
cat > "$TMP/odr_up.lk" <<'LK'
route("GET","/u", function(){ response::json(["ok" => true]) })
LK
cat > "$TMP/odr_call.lk" <<'LK'
use http
route("GET","/call", function(){ $r = http::get("http://127.0.0.1:9614/u"); response::json(["st" => $r["status"]]) })
LK
"$FCGI" --mode http --port 9614 "$TMP/odr_up.lk" >/dev/null 2>&1 &
OU=$!
sleep 2
LOOK_ALLOW_SSRF=1 "$FCGI" --mode http --port 9615 "$TMP/odr_call.lk" >/dev/null 2>&1 &
OC=$!
sleep 2
odr_r=$(curl -s --max-time 10 "http://127.0.0.1:9615/call")
sleep 1
odr_alive=no
curl -s -o /dev/null --max-time 5 "http://127.0.0.1:9615/call" && odr_alive=yes
kill $OC $OU 2>/dev/null; sleep 1; kill -9 $OC $OU 2>/dev/null
[ "$odr_r" = '{"st":200}' ] || { echo "FAIL: web route'ta http::get bozuk: [$odr_r] (ODR/crash?)"; fail=1; }
[ "$odr_alive" = "yes" ] || { echo "FAIL: http::get sonrasi SUNUCU COKTU (look::HttpResponse ODR ihlali geri geldi mi?)"; fail=1; }
if [ $fail -eq 0 ]; then
  echo "PASS: tree-walk == CLI-VM == web-VM (3 motor x 20 kategori + CLI print/exit + fallback-gurultulu)"
  echo "  $TREE"
  exit 0
else
  echo "  tree-walk: $TREE"
  echo "  CLI-VM   : $CLIVM"
  echo "  web-VM   : $WEBVM"
  exit 1
fi

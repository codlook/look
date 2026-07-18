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

# ── TOP-LEVEL bileşik atama (+= -= *= /= %= .=) — GLOBAL kapsam ──────────────────
# NEDEN AYRI: differential gövdesi run_all() FONKSİYONU içinde çalışır → oradaki
# bileşik atamalar fonksiyon-local yolunu test eder ve o yol ZATEN doğruydu.
# Bug compiler'ın TOP-LEVEL GLOBAL dalındaydı: e.op yok sayılıyordu → "$t += $i"
# sadece "$t = $i" derleniyordu. Sonuç: interpreter DOĞRU, VM SESSİZCE YANLIŞ
# (toplam/string birikimi bozuk, hata yok). Bu yüzden kontrol TOP-LEVEL olmalı —
# gövdeye eklemek bug'ı YAKALAMAZDI. (Guard'ın kapsamadığı yüzey = bug'ın yeri.)
cat > "$TMP/ca.lk" <<'LK'
$a = "x"
$a .= "y"
$n = 1
$n += 2
$m = 10
$m -= 3
$p = 4
$p *= 3
$q = 12
$q /= 4
$r = 17
$r %= 5
$t = 0
$i = 0
while ($i < 5) { $t += $i; $i = $i + 1 }
print($a . "|" . $n . "|" . $m . "|" . $p . "|" . $q . "|" . $r . "|" . $t)
LK
ca_tree=$(LOOK_CLI_VM=0 "$LK" "$TMP/ca.lk" 2>&1)
ca_vm=$("$LK" "$TMP/ca.lk" 2>&1)
ca_bek="xy|3|7|12|3|2|10"
[ "$ca_tree" = "$ca_bek" ] || { echo "FAIL: top-level compound tree-walk: [$ca_tree] (beklenen $ca_bek)"; fail=1; }
[ "$ca_vm"   = "$ca_bek" ] || { echo "FAIL: top-level compound CLI-VM: [$ca_vm] (beklenen $ca_bek) — compiler global dalinda e.op yok sayiliyor olabilir"; fail=1; }

# ── TEMPLATE motoru: dil semantigiyle AYNI olmali ────────────────────────────────
# NEDEN: sablon motoru truthiness'i ve float bicimlemesini DILDEN KOPYALAMISTI ve
# ikisinde de sapmisti:
#   {#if "0"}      → sablon TRUE, dil FALSE  (DB'den string "0" gelen alanlar!)
#   {$1234567.5}   → sablon "1.23457e+06" (bilimsel + HASSASIYET KAYBI), dil "1234567.5"
# Ikisi de SESSIZ: hata yok, yalnizca yanlis dal / yanlis sayi. Sablon motorunun
# guard'i HIC YOKTU — bu yuzden kacti. Artik dile delege ediyor; bu kontrol kilitler.
cat > "$TMP/tpl.html" <<'TPLEOF'
if0:{#if s0}T{#else}F{/if} ifs:{#if ss}T{#else}F{/if} f:{$big} esc:{$xss}
TPLEOF
cat > "$TMP/tpl.lk" <<'TPLLK'
use template
function b($v) { if ($v) { return "T" } return "F" }
$s0 = "0"
$ss = "x"
$big = 1234567.5
$xss = "<b>&'''\"" 
print("KOD:if0:" . b($s0) . " ifs:" . b($ss) . " f:" . $big)
print(template::render("tpl", ["s0"=>$s0,"ss"=>$ss,"big"=>$big,"xss"=>$xss]))
TPLLK
tpl_out=$(cd "$TMP" && LOOK_CLI_VM=0 "$LK" tpl.lk 2>&1)
tpl_vm=$(cd "$TMP" && "$LK" tpl.lk 2>&1)
echo "$tpl_out" | grep -q 'KOD:if0:F ifs:T f:1234567.5' || { echo "FAIL: template guard — KOD satiri beklenmedik: [$tpl_out]"; fail=1; }
echo "$tpl_out" | grep -q 'if0:F ifs:T f:1234567.5' || { echo "FAIL: TEMPLATE dil semantiginden SAPIYOR (truthiness \"0\" veya float formati): [$tpl_out]"; fail=1; }
echo "$tpl_out" | grep -q 'esc:&lt;b&gt;&amp;' || { echo "FAIL: template HTML escaping bozuk (XSS riski): [$tpl_out]"; fail=1; }
[ "$tpl_out" = "$tpl_vm" ] || { echo "FAIL: template tree-walk != CLI-VM"; fail=1; }

# ── array::set — SAYISAL dizide veri yok ediyordu ────────────────────────────────
# NEDEN: "Convert numeric array to assoc" dali donusturmuyor, DEGISTIRIYORDU —
# yalniz yeni anahtar/degeri iceren yepyeni bir assoc kurup orijinal elemanlari
# atiyordu. GECERLI indekste bile:
#   array::set([1,2,3], 1, "X") = {"1":"X"}   (beklenen [1,"X",3])
# Sessiz veri kaybi: hata yok, count degisiyor, eski elemanlar erisilemez.
# Guard'da array::set HIC yoktu (assoc yolu dogruydu, yalniz o kullaniliyordu).
# Sozlesme: gecerli indeks → yerinde degistir; size'a esit → sona ekle; string
# anahtar / aralik disi → assoc'a donustur ama MEVCUT ELEMANLARI KORU.
cat > "$TMP/aset.lk" <<'LK'
use array
print(json::encode(array::set([1,2,3], 1, "X")) . "|" .
      json::encode(array::set([1,2,3], 3, "X")) . "|" .
      json::encode(array::set([1,2,3], "k", "X")) . "|" .
      json::encode(array::set([], "k", "X")) . "|" .
      json::encode(array::set(["a"=>1,"b"=>2], "a", 9)))
LK
as_tree=$(LOOK_CLI_VM=0 "$LK" "$TMP/aset.lk" 2>&1)
as_vm=$("$LK" "$TMP/aset.lk" 2>&1)
as_bek='[1,"X",3]|[1,2,3,"X"]|{"0":1,"1":2,"2":3,"k":"X"}|{"k":"X"}|{"a":9,"b":2}'
[ "$as_tree" = "$as_bek" ] || { echo "FAIL: array::set tree-walk: [$as_tree] (beklenen $as_bek) — sayisal dizide veri yok ediliyor olabilir"; fail=1; }
[ "$as_vm"   = "$as_bek" ] || { echo "FAIL: array::set CLI-VM: [$as_vm] (beklenen $as_bek)"; fail=1; }

# ── $arr["anahtar"] — SAYISAL listeye string anahtar ─────────────────────────────
# NEDEN: iki motor da yanlisti, farkli sekilde (guard'da bu vaka HIC yoktu):
#   VM        : listeyi cift listesi sanip basina sentinel ekliyordu ->
#               $a=[1,2,3]; $a["k"]="X"  =>  {"1":2,"3":"k"}  TUM VERI YOK OLUYOR,
#               yazilan deger bile erisilemez ($a["k"] -> null)
#   tree-walk : assoc dalina girmeyip sayisal dala dusuyor, to_int("k")=0 ->
#               ["X",2,3]  yanlis eleman eziliyor, anahtar kayboluyor
# Ikisi de SESSIZ. Sozlesme: listeyi assoc'a donustur ama MEVCUT ELEMANLARI
# sayisal indeksleriyle anahtarlayarak KORU (array::set ile ayni).
# Ayrica "1" gibi TAM SAYI metni sayisal indekstir, assoc anahtari degil —
# eskiden VM assoc'a cevirip tree-walk indekse yaziyordu (ayrisma).
cat > "$TMP/ikey.lk" <<'LK'
$a = [1,2,3]
$a["k"] = "X"
$b = []
$b["k"] = "X"
$c = [1,2,3]
$c["1"] = "X"
$d = [1,2,3]
print(json::encode($a) . "|" . $a["k"] . "|" . $a["1"] . "|" .
      json::encode($b) . "|" . json::encode($c) . "|" .
      json::encode($d["k"]) . "|" . $d["1"])
LK
ik_tree=$(LOOK_CLI_VM=0 "$LK" "$TMP/ikey.lk" 2>&1)
ik_vm=$("$LK" "$TMP/ikey.lk" 2>&1)
ik_bek='{"0":1,"1":2,"2":3,"k":"X"}|X|2|{"k":"X"}|[1,"X",3]|null|2'
[ "$ik_tree" = "$ik_bek" ] || { echo "FAIL: \$arr[str] tree-walk: [$ik_tree] (beklenen $ik_bek) — string anahtar sayisal indekse zorlaniyor olabilir"; fail=1; }
[ "$ik_vm"   = "$ik_bek" ] || { echo "FAIL: \$arr[str] CLI-VM: [$ik_vm] (beklenen $ik_bek) — liste assoc'a cevrilirken elemanlar korunmuyor olabilir"; fail=1; }


# ── string::replace — BOS arama dizesi ASIYORDU (DoS) ───────────────────────────
# NEDEN: s.find("", pos) her zaman pos'u dondurur -> dongu her turda araya 'to'
# ekliyor, string surekli buyuyor: SONSUZ DONGU + SINIRSIZ BELLEK. Web'de
# string::replace($metin, $kullaniciGirdisi, $x) cagrisinda kullanici bos string
# gonderirse worker sonsuza kadar kilitleniyordu — tek istekle DoS.
# Sozlesme: Go (strings.Replace(s,"",new,-1)) — bos arama dizesi metnin BASINDA
# ve her UTF-8 dizisinden SONRA eslesir, k kod noktasi icin k+1 ekleme.
# TIMEOUT ile kosuluyor: asilma regresyonu FAIL olarak gorunsun (sessizce
# bekleyip CI'yi kilitlemesin).
cat > "$TMP/rep.lk" <<'LK'
use string
print(string::replace("abc", "", "X") . "|" . string::replace("abc", "", "") . "|" .
      string::replace("şğ", "", "-") . "|" . string::replace("", "", "X") . "|" .
      string::replace("aXbXc", "X", "-"))
LK
rp_bek='XaXbXcX|abc|-ş-ğ-|X|a-b-c'
rp_tree=$(timeout 10 env LOOK_CLI_VM=0 "$LK" "$TMP/rep.lk" 2>&1); rp_rc=$?
[ $rp_rc -eq 124 ] && { echo "FAIL: string::replace tree-walk ASILDI (bos arama dizesi sonsuz dongu)"; fail=1; }
rp_vm=$(timeout 10 "$LK" "$TMP/rep.lk" 2>&1); rp_rc2=$?
[ $rp_rc2 -eq 124 ] && { echo "FAIL: string::replace CLI-VM ASILDI (bos arama dizesi sonsuz dongu)"; fail=1; }
[ $rp_rc -eq 124 ] || [ "$rp_tree" = "$rp_bek" ] || { echo "FAIL: string::replace tree-walk: [$rp_tree] (beklenen $rp_bek)"; fail=1; }
[ $rp_rc2 -eq 124 ] || [ "$rp_vm" = "$rp_bek" ] || { echo "FAIL: string::replace CLI-VM: [$rp_vm] (beklenen $rp_bek)"; fail=1; }


# ── string::pad_* — GECERSIZ UTF-8 uretiyordu + bayt/kod-noktasi tutarsizligi ───
# NEDEN (uc bug tek kokten):
#   1) Uzun metni KIRPIYORDU: pad_left("abcdef",3,"0") = "def" (sessiz veri kaybi;
#      PHP str_pad / Python ljust / Go fmt kirpmaz — genislik ASGARI'dir)
#   2) Kirpma BAYT tabanliydi -> cok baytli karakteri ortadan boluyordu:
#      pad_left("şğü",3,"x") = 0x9F 0xC3 0xBC — oksuz devam baytiyla baslayan
#      GECERSIZ UTF-8. Bozuk metin JSON'a/DB'ye/HTTP yanitina aynen gidiyordu.
#   3) Genislik BAYT sayiyordu, oysa len/substr/upper/reverse kod noktasi sayar:
#      pad_left("ş",3,"x") = "xş", gorunen uzunluk 2 (beklenen 3) — modul kendi
#      icinde tutarsizdi.
# Sozlesme: Go fmt "%Ns" — genislik ASGARI ve KOD NOKTASI. Kirpma yok.
# Guard cikti gecerli UTF-8 mi diye de bakar (iconv) — 2. bugin dogrudan kilidi.
cat > "$TMP/pad.lk" <<'LK'
use string
print(string::pad_left("ş",3,"x") . "|" . string::len(string::pad_left("ş",3,"x")) . "|" .
      string::pad_right("ş",3,"x") . "|" . string::pad_left("a",4,"ş") . "|" .
      string::pad_left("abcdef",3,"0") . "|" . string::pad_left("şğü",3,"x") . "|" .
      string::pad_left("",3,"x") . "|" . string::pad_left("ab",0,"x"))
LK
pd_bek='xxş|3|şxx|şşşa|abcdef|şğü|xxx|ab'
pd_tree=$(timeout 10 env LOOK_CLI_VM=0 "$LK" "$TMP/pad.lk" 2>&1)
pd_vm=$(timeout 10 "$LK" "$TMP/pad.lk" 2>&1)
[ "$pd_tree" = "$pd_bek" ] || { echo "FAIL: string::pad_* tree-walk: [$pd_tree] (beklenen $pd_bek) — bayt tabanli genislik veya kirpma geri gelmis olabilir"; fail=1; }
[ "$pd_vm"   = "$pd_bek" ] || { echo "FAIL: string::pad_* CLI-VM: [$pd_vm] (beklenen $pd_bek)"; fail=1; }
if command -v iconv >/dev/null 2>&1; then
  printf '%s' "$pd_vm" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 || { echo "FAIL: string::pad_* GECERSIZ UTF-8 uretiyor (cok baytli karakter ortadan bolunuyor)"; fail=1; }
fi


# ── string::upper/lower — "global" iddiasi + Turkce locale OLMAMASI ─────────────
# IKI YONLU KILIT:
# 1) TURKCE LOCALE YOK (dil global): upper("i") = "I" olmali, "İ" DEGIL.
#    ı → I, İ → i (standart Unicode). Bu bilincli bir karar — Latin Ext-A'nin
#    genel cift/tek kurali ı(0x131) -> İ(0x130) verir, yani kural yanlis sirayla
#    yazilirsa Turkce locale SESSIZCE geri gelir. Bu kontrol onu yakalar.
# 2) KAPSAM gercekten global mi: eskiden tablo ASCII + Latin-1 + 3 Turkce kod
#    noktasindan ibaretti; Kiril/Yunan/Latin Ext-A sessizce donusmeden geciyordu
#    (upper("привет") = "привет", upper("łódź") = "łÓDź" — YARIM donusum).
cat > "$TMP/case.lk" <<'LK'
use string
print(string::upper("i") . string::lower("I") . string::upper("ı") . string::lower("İ") . "|" .
      string::upper("çğöşü") . "|" . string::upper("привет") . "|" . string::lower("ПРИВЕТ") . "|" .
      string::upper("ελλάδα") . "|" . string::upper("łódź") . "|" . string::upper("čeština") . "|" .
      string::lower(string::upper("привет")))
LK
cs_bek='IiIi|ÇĞÖŞÜ|ПРИВЕТ|привет|ΕΛΛΆΔΑ|ŁÓDŹ|ČEŠTINA|привет'
cs_tree=$(timeout 10 env LOOK_CLI_VM=0 "$LK" "$TMP/case.lk" 2>&1)
cs_vm=$(timeout 10 "$LK" "$TMP/case.lk" 2>&1)
[ "$cs_tree" = "$cs_bek" ] || { echo "FAIL: string::upper/lower tree-walk: [$cs_tree] (beklenen $cs_bek) — Turkce locale geri gelmis veya kapsam eksik olabilir"; fail=1; }
[ "$cs_vm"   = "$cs_bek" ] || { echo "FAIL: string::upper/lower CLI-VM: [$cs_vm] (beklenen $cs_bek)"; fail=1; }


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
  echo "PASS: tree-walk == CLI-VM == web-VM (3 motor x 22 kategori + CLI print/exit + fallback-gurultulu)"
  echo "  $TREE"
  exit 0
else
  echo "  tree-walk: $TREE"
  echo "  CLI-VM   : $CLIVM"
  echo "  web-VM   : $WEBVM"
  exit 1
fi

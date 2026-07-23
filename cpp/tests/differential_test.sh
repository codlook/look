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
# SAYISAL indeks uclari — VM tree-walk'a (referans semantik) hizalandi.
# ESKI HATA: VM aralik disinda ve negatifte SESSIZDI, interpreter ise hata
# firlatiyor/sondan sayiyordu — ayni program iki motorda iki farkli sonuc:
#   $a[99] oku    -> VM null / tree-walk HATA
#   $a[-1] oku    -> VM null / tree-walk 3
#   $a[99]="X"    -> VM atamayi SESSIZCE ATLIYOR / tree-walk HATA
#   $a[-1]="X"    -> VM atamayi SESSIZCE ATLIYOR / tree-walk [1,2,"X"]
# "Yazdim ama yazilmadi" ve "okudum, null geldi (oysa hata olmaliydi)" ayni
# sinif: sessiz yanlis veri. Sozlesme: negatif = sondan, aralik disi = HATA,
# size'a esit yazma = sona ekleme.
cat > "$TMP/inum.lk" <<'LK'
function t($f) { try { return json::encode($f()) } catch ($e) { return "HATA" } }
print(t(function(){ $a=[1,2,3]; return $a[-1] }) . "|" .
      t(function(){ $a=[1,2,3]; return $a[-4] }) . "|" .
      t(function(){ $a=[1,2,3]; return $a[99] }) . "|" .
      t(function(){ $a=[1,2,3]; $a[-1]="X"; return $a }) . "|" .
      t(function(){ $a=[1,2,3]; $a[3]="X"; return $a }) . "|" .
      t(function(){ $a=[1,2,3]; $a[99]="X"; return $a }))
LK
in_bek='3|HATA|HATA|[1,2,"X"]|[1,2,3,"X"]|HATA'
in_tree=$(LOOK_CLI_VM=0 "$LK" "$TMP/inum.lk" 2>&1)
in_vm=$("$LK" "$TMP/inum.lk" 2>&1)
[ "$in_tree" = "$in_bek" ] || { echo "FAIL: \$arr[sayi] uclari tree-walk: [$in_tree] (beklenen $in_bek)"; fail=1; }
[ "$in_vm"   = "$in_bek" ] || { echo "FAIL: \$arr[sayi] uclari CLI-VM: [$in_vm] (beklenen $in_bek) — VM aralik disinda/negatifte SESSIZ olabilir"; fail=1; }

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


# ── GUVENLIK: request::ip() sahtelenemez mi? ────────────────────────────────────
# NEDEN: ctx.remote_addr yalnizca X-Forwarded-For basligindan dolduruluyordu ve
# guvenilir-proxy kontrolu YOKTU. Istemci kendi IP'sini seciyordu:
#   curl -H "X-Forwarded-For: 9.9.9.9"  ->  request::ip() = "9.9.9.9"
# IP'ye dayali yetkilendirme, ban listesi ve rate-limit bypass edilebiliyordu.
# Baslik hic yoksa da "" donuyordu (gercek peer yok sayiliyor).
# Sozlesme: GERCEK TCP peer esastir; X-Real-IP / X-Forwarded-For'a yalnizca
# baglanan IP LOOK_TRUSTED_PROXY listesindeyse bakilir; zincirin ILKI alinir.
# Bu kontrol LOOK_TRUSTED_PROXY OLMADAN kosar: baslik ne gonderilirse gonderilsin
# sonuc gercek peer olmali.
IP_PORT=9616
cat > "$TMP/ip.lk" <<'LK'
route("GET", "/ip", function(){ return response::text(request::ip()) })
LK
"$FCGI" --mode http --port $IP_PORT "$TMP/ip.lk" >/dev/null 2>&1 &
IPPID=$!
for i in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$IP_PORT/ip" && break; sleep 0.3; done
ip_plain=$(curl -s "http://127.0.0.1:$IP_PORT/ip")
ip_spoof=$(curl -s "http://127.0.0.1:$IP_PORT/ip" -H "X-Forwarded-For: 9.9.9.9")
ip_chain=$(curl -s "http://127.0.0.1:$IP_PORT/ip" -H "X-Forwarded-For: 1.1.1.1, 2.2.2.2")
ip_real=$(curl -s "http://127.0.0.1:$IP_PORT/ip" -H "X-Real-IP: 8.8.8.8")
kill $IPPID 2>/dev/null; wait $IPPID 2>/dev/null
[ "$ip_plain" = "127.0.0.1" ] || { echo "FAIL: request::ip() baslik yokken gercek peer'i vermiyor: [$ip_plain]"; fail=1; }
[ "$ip_spoof" = "127.0.0.1" ] || { echo "FAIL: GUVENLIK — request::ip() SAHTELENEBILIYOR: X-Forwarded-For ile [$ip_spoof] donduruldu (guvenilir proxy tanimli degilken gercek peer olmaliydi)"; fail=1; }
[ "$ip_chain" = "127.0.0.1" ] || { echo "FAIL: GUVENLIK — request::ip() XFF zincirine guveniyor: [$ip_chain]"; fail=1; }
[ "$ip_real"  = "127.0.0.1" ] || { echo "FAIL: GUVENLIK — request::ip() X-Real-IP'ye guveniyor: [$ip_real]"; fail=1; }


# ── GUVENLIK: session verisi enjekte edilebiliyor mu? ───────────────────────────
# NEDEN: session blob'u satir tabanli "anahtar=deger\n" formatinda ve anahtar/deger
# HIC KACIRILMIYORDU -> degerin icindeki \n YENI BIR ALAN tanimliyordu.
# Uygulama kullanici verisini oturuma yaziyorsa (isim/not/arama — cok yaygin):
#   ?isim=bob\nrol=admin\nadmin=1  ->  session::get("admin") = "1"
# HIC VAR OLMAYAN yetki alani enjekte ediliyordu (login oncesi istekte bile).
# Sozlesme: kacirarak sakla — mesru satir sonu VERI olarak korunur, ayrac anlami
# tasimaz. Guard hem saldiriyi hem gidis-donusu kontrol eder.
SS_PORT=9617
cat > "$TMP/sess.lk" <<'LK'
route("GET", "/s", function(){
  session::start()
  session::set("rol", "uye")
  session::set("isim", request::get("isim"))
  return response::text(json::encode(session::get("rol")) . "|" . json::encode(session::get("admin")))
})
LK
"$FCGI" --mode http --port $SS_PORT "$TMP/sess.lk" >/dev/null 2>&1 &
SSPID=$!
for i in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$SS_PORT/s?isim=x" && break; sleep 0.3; done
ss_out=$(curl -s -c "$TMP/ck" -b "$TMP/ck" --get --data-urlencode 'isim=bob
rol=admin
admin=1' "http://127.0.0.1:$SS_PORT/s")
kill $SSPID 2>/dev/null; wait $SSPID 2>/dev/null
[ "$ss_out" = '"uye"|null' ] || { echo "FAIL: GUVENLIK — session verisi ENJEKTE EDILEBILIYOR: [$ss_out] (beklenen \"uye\"|null; deger icindeki satir sonu yeni alan tanimliyor olabilir)"; fail=1; }


# ── GUVENLIK: cookie::set oznitelik enjeksiyonu ─────────────────────────────────
# NEDEN: sanitize_header() yalnizca CR/LF/NUL temizliyordu; cerezde ';' YAPISAL
# AYRAC oldugu icin kullanici kontrolundeki deger oznitelik enjekte edebiliyordu:
#   ?v=x; HttpOnly; Domain=evil.com
#   -> Set-Cookie: tercih=x; HttpOnly; Domain=evil.com; Path=/
# Domain'i ust alan adina cekip cerezi tum alt alan adlarina sizdirmak mumkundu.
# Sozlesme: PHP setcookie() gibi — yazarken yuzde-kodla, okurken coz. Mesru
# deger korunur ama yapi anlami tasiyamaz.
CK_PORT=9618
cat > "$TMP/ck.lk" <<'LK'
route("GET", "/c", function(){ cookie::set("tercih", request::get("v")); return response::text("ok") })
route("GET", "/r", function(){ return response::text(json::encode(cookie::get("tercih"))) })
LK
"$FCGI" --mode http --port $CK_PORT "$TMP/ck.lk" >/dev/null 2>&1 &
CKPID=$!
for i in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$CK_PORT/r" && break; sleep 0.3; done
ck_hdr=$(curl -s -D - -o /dev/null "http://127.0.0.1:$CK_PORT/c?v=x%3B%20HttpOnly%3B%20Domain%3Devil.com" | grep -i "^set-cookie")
ck_rt=$(curl -s -c "$TMP/ckj" "http://127.0.0.1:$CK_PORT/c?v=a%3Bb" >/dev/null; curl -s -b "$TMP/ckj" "http://127.0.0.1:$CK_PORT/r")
kill $CKPID 2>/dev/null; wait $CKPID 2>/dev/null
# DIKKAT: kelimeyi basligin tamaminda ARAMA — kodlanmis deger "HttpOnly" metnini
# duz metin olarak icerir (x%3B%20HttpOnly%3B%20...) ve bu GUVENLIDIR. Enjeksiyon
# ancak GERCEK ';' ayraciyla olur, o yuzden OZNITELIK BICIMI aranir.
echo "$ck_hdr" | grep -qi "; *Domain=" && { echo "FAIL: GUVENLIK — cookie::set OZNITELIK ENJEKTE EDILEBILIYOR: [$ck_hdr]"; fail=1; }
echo "$ck_hdr" | grep -qi "; *HttpOnly" && { echo "FAIL: GUVENLIK — cookie degeri ';' ile oznitelik ekleyebiliyor: [$ck_hdr]"; fail=1; }
[ "$ck_rt" = '"a;b"' ] || { echo "FAIL: cookie gidis-donus bozuk: [$ck_rt] (beklenen \"a;b\" — kodlama var ama cozme yok olabilir)"; fail=1; }


# ── db:: parametre baglama — literal icindeki '?' + sayi uyusmazligi ────────────
# NEDEN: bind_params TUM '?' karakterlerini kor sekilde degistiriyordu; string
# literali/tanimlayici/yorum icindeki '?' de placeholder saniliyordu:
#   "SELECT 'Hazir mi?' AS s, ? AS p" + ["DEGER"]
#   -> SELECT 'Hazir mi'DEGER'' AS s, ? AS p   (literal bozuldu, param kaydi)
# Icinde soru isareti gecen HERHANGI bir metin SQL literalinde kullanilamiyordu
# (Turkce cumlelerde cok yaygin; PostgreSQL'de '?' ayrica JSONB operatorudur).
# Parametre sayisi uyusmazligi da IKI YONDE SESSIZDI: fazla param atiliyordu,
# eksik param '?' birakiyor ve SQLite onu kendi placeholder'i sayip NULL veriyordu.
# Sozlesme: '?' yalnizca SQL GOVDESINDE placeholder; sayi uyusmazligi HATA (Go'da
# db.Query da hata doner). SQL ENJEKSIYONU kontrolu de burada kilitleniyor.
cat > "$TMP/db.lk" <<'LK'
$c = db::connect("sqlite://:memory:")
db::exec($c, "CREATE TABLE k (ad TEXT)")
db::exec($c, "INSERT INTO k (ad) VALUES (?)", ["ali"])
$lit = db::query($c, "SELECT 'Hazir mi?' AS s, ? AS p", ["D"])
$inj = db::query($c, "SELECT * FROM k WHERE ad = ?", ["' OR '1'='1"])
$hep = db::query($c, "SELECT * FROM k")
$mis = "yok"
try { db::query($c, "SELECT ? AS a", ["bir", "fazla"]); $mis = "SESSIZ" } catch ($e) { $mis = "hata" }
$mis2 = "yok"
try { db::query($c, "SELECT ? AS a, ? AS b", ["tek"]); $mis2 = "SESSIZ" } catch ($e) { $mis2 = "hata" }
print($lit[0].s . "|" . $lit[0].p . "|" . count($inj) . "|" . count($hep) . "|" . $mis . "|" . $mis2)
LK
db_out=$(timeout 20 "$LK" "$TMP/db.lk" 2>&1 | grep -v '^\[20')
db_bek='Hazir mi?|D|0|1|hata|hata'
[ "$db_out" = "$db_bek" ] || { echo "FAIL: db:: parametre baglama: [$db_out] (beklenen $db_bek) — literal icindeki '?' placeholder saniliyor veya sayi uyusmazligi SESSIZ olabilir"; fail=1; }


# ── HATA MESAJI paritesi — 'use' unutuldugunda ──────────────────────────────────
# NEDEN: modul 'use' edilmemisse VM builtin tablosundaki giris BOS std::function'dir;
# cagrilinca C++ std::bad_function_call firlatiyor ve kullaniciya OLDUGU GIBI
# ciktiyordu:
#   print(string::len("abc"))  ->  "Runtime Error: bad_function_call"
# Ne modulu ne fonksiyonu soyluyor, ustelik bir C++ ic terimi. tree-walk (REFERANS)
# ise dogru mesaji veriyordu: "Module 'string' not loaded."
# 'use' unutmak LOOK'ta EN SIK yapilan hata, yani VARSAYILAN motor en kotu mesaji
# veriyordu. Hata metni de sozlesmenin parcasidir (S3): iki motor ayni demeli.
cat > "$TMP/uerr.lk" <<'LK'
print(string::len("abc"))
LK
cat > "$TMP/uok.lk" <<'LK'
use string
print(string::len("abc"))
LK
ue_tree=$(LOOK_CLI_VM=0 "$LK" "$TMP/uerr.lk" 2>&1 | grep -i "error" | head -1)
ue_vm=$("$LK" "$TMP/uerr.lk" 2>&1 | grep -i "error" | head -1)
uo_vm=$("$LK" "$TMP/uok.lk" 2>&1 | tail -1)
echo "$ue_vm" | grep -q "bad_function_call" && { echo "FAIL: 'use' unutulunca VM C++ ic istisnasini sizdiriyor: [$ue_vm]"; fail=1; }
[ "$ue_tree" = "$ue_vm" ] || { echo "FAIL: hata mesaji motorlar arasi ayrisiyor — tree-walk: [$ue_tree] / CLI-VM: [$ue_vm]"; fail=1; }
[ "$uo_vm" = "3" ] || { echo "FAIL: 'use' VARKEN modul cagrisi bozuldu (pozitif kontrol): [$uo_vm]"; fail=1; }

# Cagrilamayan AD — mesaj adi soylemeli ve iki motor ayni demeli.
# ESKI HATA (iki yonlu): VM adi bilmiyordu ("Cagrilabilir degil (BYTECODE_FN
# bekleniyor)" — hangi ad?); tree-walk ise her durumda "is not defined" diyordu,
# oysa deger TANIMLI olabilir ve yalnizca cagrilabilir degildir ($x = 5; $x(1)).
# Yani REFERANS MOTOR da yaniliyordu. Ad, compiler tarafindan NOP hint'ine
# gomulup VM'e tasiniyor (CALL_BUILTIN'in 16-bit indeks kalibi).
printf 'print(olmayan_fonksiyon(1))\n' > "$TMP/c1.lk"
printf '$x = 5\nprint($x(1))\n'        > "$TMP/c2.lk"
for cf in c1 c2; do
  cv=$("$LK" "$TMP/$cf.lk" 2>&1 | grep -i error | head -1)
  ct=$(LOOK_CLI_VM=0 "$LK" "$TMP/$cf.lk" 2>&1 | grep -i error | head -1)
  [ "$cv" = "$ct" ] || { echo "FAIL: cagri hatasi motorlar arasi ayrisiyor ($cf) — CLI-VM: [$cv] / tree-walk: [$ct]"; fail=1; }
  echo "$cv" | grep -q "BYTECODE_FN" && { echo "FAIL: cagri hatasi ic terim sizdiriyor (ad soylenmiyor): [$cv]"; fail=1; }
done
echo "$(LOOK_CLI_VM=0 "$LK" "$TMP/c2.lk" 2>&1)" | grep -q "is not defined" && {
  echo "FAIL: tanimli ama cagrilamayan deger icin yaniltici 'is not defined' mesaji"; fail=1; }


# ── PostgreSQL wire — BOZUK DataRow sessizce yanlis veri uretiyor mu? ───────────
# NEDEN: postgres_client.cpp'nin (1023 satir) HIC guard'i yoktu. Sahte sunucuyla
# olculdu — bozuk DataRow'da LOOK sessizce yanlis veri donduruyordu:
#   uzunluk 100 der, 2 bayt gonderir  -> alan "" (bos string), HATA YOK
#   int32-max uzunluk                 -> alan "" , HATA YOK
#   3 alan vaat eder, 1 tane gonderir -> satir 1 sutunla doner, HATA YOK
# Cokme yoktu (sinir kontrolu tutuyordu) ama uygulama bos degeri gercek veri
# sanip ona gore davraniyordu. Sessiz yanlis veri bu projede cokmeden tehlikeli.
# Ayrica 'p + field_len <= end' isaretci TASMASINA acikti.
# Sozlesme: bozuk DataRow -> NET HATA; gecerli satir bozulmadan calisir.
if command -v python3 >/dev/null 2>&1 && [ -f "$(dirname "$0")/fake_pg_server.py" ]; then
  FPG="$(dirname "$0")/fake_pg_server.py"
  cat > "$TMP/pg.lk" <<'LK'
try {
  $c = db::connect(env("PGDSN", ""))
  print("SONUC=" . json::encode(db::query($c, "SELECT col FROM t")))
} catch ($e) { print("HATA") }
LK
  pg_sonuc=""
  for mod in saglam truncated negatif dev_uzunluk eksik_alan; do
    pgport=$((55700 + RANDOM % 200))
    python3 "$FPG" $mod $pgport > "$TMP/fpg.log" 2>&1 &
    fpgpid=$!
    for i in $(seq 1 25); do grep -q hazir "$TMP/fpg.log" 2>/dev/null && break; sleep 0.2; done
    out=$(PGDSN="postgres://u:p@127.0.0.1:$pgport/test" timeout 15 "$LK" "$TMP/pg.lk" 2>&1 | grep -E '^(SONUC|HATA)' | head -1)
    kill $fpgpid 2>/dev/null; wait $fpgpid 2>/dev/null
    pg_sonuc="$pg_sonuc${out%%=*}|"
  done
  # saglam=SONUC, digerlerinin HEPSI hata vermeli
  pg_bek='SONUC|HATA|HATA|HATA|HATA|'
  [ "$pg_sonuc" = "$pg_bek" ] || { echo "FAIL: PostgreSQL wire — bozuk DataRow sessizce veri donduruyor olabilir: [$pg_sonuc] (beklenen $pg_bek)"; fail=1; }
else
  echo "  (atlandi: PostgreSQL wire testi python3 gerektirir)"
fi


# ── MOD PARITESI: multipart yukleme --mode http'de de calisiyor mu? ─────────────
# NEDEN: govde ayristirma UC yerde ayri yaziliydi ve http_main'deki kopyada
# multipart dali HIC YOKTU -> request::file() FastCGI'de calisiyor, --mode http'de
# "requires multipart/form-data request" hatasi veriyordu. Ayni uygulama moda gore
# farkli davraniyordu (S12). README'de "bilinen sinir" diye yaziliydi; oysa
# unutulmus bir daldi. Ayrica http_main method kapisi koyuyordu (POST-only),
# FastCGI koymuyordu -> PUT/PATCH govdeleri de modlara gore ayrisiyordu.
# Sozlesme: govde ayristirma TEK kaynaktan (WebContext::parse_post_body).
MP_PORT=9619
cat > "$TMP/up.lk" <<'LK'
route("POST","/u", function(){
  try { $f = request::file("dosya")
        return response::text("OK|" . $f.size . "|" . request::post("not")) }
  catch ($e) { return response::text("HATA") }
})
route("PUT","/p", function(){ return response::text(json::encode(request::post("a"))) })
LK
printf 'multipart test icerigi\n' > "$TMP/mp.txt"
"$FCGI" --mode http --port $MP_PORT "$TMP/up.lk" >/dev/null 2>&1 &
MPPID=$!
for i in $(seq 1 30); do curl -s -o /dev/null -X PUT "http://127.0.0.1:$MP_PORT/p" -d "a=1" && break; sleep 0.3; done
mp_out=$(curl -s -X POST "http://127.0.0.1:$MP_PORT/u" -F "dosya=@$TMP/mp.txt" -F "not=merhaba")
put_out=$(curl -s -X PUT "http://127.0.0.1:$MP_PORT/p" -d "a=putdan")
kill $MPPID 2>/dev/null; wait $MPPID 2>/dev/null
[ "$mp_out" = "OK|23|merhaba" ] || { echo "FAIL: --mode http'de multipart/request::file calismiyor: [$mp_out] (beklenen OK|23|merhaba — 23 = "multipart test icerigi" + satir sonu)"; fail=1; }
[ "$put_out" = '"putdan"' ] || { echo "FAIL: --mode http'de PUT govdesi ayrismiyor (FastCGI ile parite yok): [$put_out]"; fail=1; }


# ── http:: SSRF + IPv6 URL ayristirma ───────────────────────────────────────────
# IKI SEY KILITLENIYOR:
# 1) SSRF ENGELI ile GERCEK AG HATASI ayirt edilebilmeli. tcp_connect DNS hatasi,
#    SSRF engeli ve baglanti hatasi icin ayni INVALID'i donduruyor, cagiran da
#    hepsine "connection failed" diyordu -> ic servisine ulasamayan gelistirici
#    bir GUVENLIK POLITIKASININ engelledigini goremiyordu.
# 2) IPv6 literal URL'ler HIC CALISMIYORDU: parse_url kosleli parantezleri
#    siyirmiyor, port ayracini rfind(':') ile ariyordu ->
#      "[::1]:8080" -> host "[::1]"  (parantezli, getaddrinfo basarisiz)
#      "[::1]"      -> host "[:"     (adresin ICINDEKI iki nokta port sanildi)
#    Hata "DNS cozumlenemedi" oldugu icin sebep gorunmuyordu; ancak (1) yapilip
#    mesajlar ayrisinca ortaya cikti.
# Bu kontrol SUNUCU GEREKTIRMEZ: SSRF korumasi ACIKKEN [::1] icin beklenen hata
# SSRF'tir. Parse tekrar bozulursa hata "DNS cozumlenemedi"ye doner ve FAIL olur.
cat > "$TMP/ssrf.lk" <<'LK'
use http
$a = http::get("http://[::1]:9/")
$b = http::get("http://127.0.0.1:9/")
$r = http::get("http://[::1:9/")
$c = $r.error
print($a.error . "|" . $b.error . "|" . $c)
LK
sf_out=$(timeout 30 "$LK" "$TMP/ssrf.lk" 2>&1 | tail -1)
# Tek regex yerine UC AYRI kosul — grep'te '|' duz karakterdir, alternation
# beklemek yanlis alarm uretir (bu kontrolu yazarken tam bunu yasadim).
sf_ok=1
if echo "$sf_out" | grep -q "DNS cozumlenemedi"; then
  echo "FAIL: IPv6 literal URL ayristirma bozuk — [::1] icin DNS hatasi doniyor,"
  echo "      demek ki kosleli parantezler siyrilmiyor (host \"[::1]\" olarak gidiyor)"
  sf_ok=0
fi
if echo "$sf_out" | grep -q "connection failed"; then
  echo "FAIL: GUVENLIK TESHISI — SSRF engeli gercek ag hatasindan ayirt edilemiyor"
  sf_ok=0
fi
echo "$sf_out" | grep -qi "ipv6" || {
  echo "FAIL: bozuk IPv6 URL ('[::1:9/') net hata vermiyor"; sf_ok=0; }
[ $sf_ok = 1 ] || { echo "  cikti: [$sf_out]"; fail=1; }


# ── KULLANICI MODÜLÜ (`use`) VM'de de yükleniyor mu? ────────────────────────────
# NEDEN: `use <ad>` iki şeyi karşılar — stdlib modülü (builtin tablosunda zaten
# bağlı) ve KULLANICI MODÜLÜ (~/.look/modules/<ad>/<ad>.lk). Compiler ikisinde de
# NOP üretiyordu; kullanıcı modülünün fonksiyonları VM globals'ında hiç oluşmuyor,
# route "Undefined variable: <fn>" verip KALICI olarak interpreter'a düşüyordu.
# Çıktı DOĞRU çıktığı için sessiz kalıyordu — fallback bug'ı maskeliyor (1. kural).
# Yani dilin "use ile çok dosyalı proje" yolu web'de HIZINI kaybediyordu.
# Bu kontrol hem çıktıyı hem "VM BUG yok"u doğrular.
UM_HOME="$TMP/umhome"
mkdir -p "$UM_HOME/.look/modules/umtest"
cat > "$UM_HOME/.look/modules/umtest/umtest.lk" <<'LK'
function um_topla($a, $b) { return $a + $b }
LK
cat > "$TMP/um.lk" <<'LK'
use umtest
route("GET","/um", function(){ return response::text("um=" . um_topla(20, 22)) })
LK
UM_PORT=9620
HOME="$UM_HOME" "$FCGI" --mode http --port $UM_PORT "$TMP/um.lk" >"$TMP/um.log" 2>&1 &
UMPID=$!
for i in $(seq 1 30); do curl -s -o /dev/null "http://127.0.0.1:$UM_PORT/um" 2>/dev/null && break; sleep 0.3; done
um_out=$(curl -s "http://127.0.0.1:$UM_PORT/um")
kill $UMPID 2>/dev/null; wait $UMPID 2>/dev/null
um_cli=$(cd "$TMP" && HOME="$UM_HOME" "$LK" -c 'use umtest
print(um_topla(20, 22))' 2>&1 | tail -1)
[ "$um_out" = "um=42" ] || { echo "FAIL: kullanici modulu web'de calismiyor: [$um_out] (beklenen um=42)"; fail=1; }
if grep -q "VM BUG" "$TMP/um.log" 2>/dev/null; then
  echo "FAIL: kullanici modulu VM'de yuklenmiyor — route KALICI interpreter'a dustu (yavas yol):"
  grep -m1 "VM BUG" "$TMP/um.log" | sed 's/^/       /'
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
  echo "PASS: tree-walk == CLI-VM == web-VM (3 motor x 22 kategori + CLI print/exit + fallback-gurultulu)"
  echo "  $TREE"
  exit 0
else
  echo "  tree-walk: $TREE"
  echo "  CLI-VM   : $CLIVM"
  echo "  web-VM   : $WEBVM"
  exit 1
fi

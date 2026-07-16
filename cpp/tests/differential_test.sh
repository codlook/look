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
cmp -s "$TMP/io_tree.out" "$TMP/io_vm.out" || {
  echo "FAIL: print/write BAYT BAYT ayrisiyor (newline / ' ' ayraci)"
  echo "  tree : $(od -c < "$TMP/io_tree.out" | head -2 | tr '\n' ' ')"
  echo "  CLIVM: $(od -c < "$TMP/io_vm.out"   | head -2 | tr '\n' ' ')"
  fail=1
}
printf 'print("basla")\nexit(3)\nprint("OLMAMALI")\n' > "$TMP/ex.lk"
LOOK_CLI_VM=0 "$LK" "$TMP/ex.lk" >/dev/null 2>&1; t_code=$?
"$LK" "$TMP/ex.lk" >/dev/null 2>&1; v_code=$?
if [ "$t_code" != "3" ] || [ "$v_code" != "3" ]; then
  echo "FAIL: exit(3) kodu — tree=$t_code CLI-VM=$v_code (ikisi de 3 olmali)"
  fail=1
fi

if [ $fail -eq 0 ]; then
  echo "PASS: tree-walk == CLI-VM == web-VM (3 motor differential + CLI print/exit)"
  echo "  $TREE"
  exit 0
else
  echo "  tree-walk: $TREE"
  echo "  CLI-VM   : $CLIVM"
  echo "  web-VM   : $WEBVM"
  exit 1
fi

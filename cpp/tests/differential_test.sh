#!/bin/bash
# TWO-COLUMN differential regression test — both must give EXACTLY the same output and match the fixed
# reference (tests/golden/differential/*.out) and the expected values (*_bek).
# Philosophy guard: the surfaces must not diverge SEMANTICALLY ("independent and stable").
#
#   1) CLI-VM     : lk <script>  — bytecode VM, main.cpp/build_cli_builtins
#   2) web-VM     : lk-fcgi --mode http (route)  — bytecode VM, http_main.cpp/req_builtins
#
# LOOK has a single engine (the interpreter was removed); there used to be a third "tree-walk" column.
# WHY 2 COLUMNS: the builtin wiring lives in TWO separate hand-written places (build_cli_builtins
# and req_builtins). They can silently drift apart — and they did (math::round
# worked on the web for a while but not in the CLI-VM). A single-column test cannot SEE this drift.
#
# Run after every engine change (opcode/builtin/Value/bridge).
# Usage: bash differential_test.sh [lk_path] [lk-fcgi_path]
set -u
# an app that uses cookies does not start without LOOK_SECRET (CSRF signature) — the test's own key.
export LOOK_SECRET="${LOOK_SECRET:-look-test-secret-0123456789abcdef0123456789}"
DIR="$(cd "$(dirname "$0")" && pwd)"
LK="${1:-$DIR/../build/lk}"
FCGI="${2:-$DIR/../build/lk-fcgi}"
# Convert to an absolute path: sub-tests cd into $TMP; the relative "build/lk" argument is not
# resolved there (the template/array::set test gave a FALSE FAIL with "No such file"). So the CI
# guard also works when given a relative path such as build/lk.
LK="$(cd "$(dirname "$LK")" && pwd)/$(basename "$LK")"
FCGI="$(cd "$(dirname "$FCGI")" && pwd)/$(basename "$FCGI")"
BODY="$DIR/differential_body.lk"
PORT=9611
TMP=$(mktemp -d)
SRV=""
# Cleanup by PID — NEVER a broad "pkill -f" (on a shared/prod server it kills other processes;
# it once caused a prod outage).
cleanup() {
  [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
  sleep 1
  [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

cp "$BODY" "$TMP/cli.lk"; echo 'print(run_all());' >> "$TMP/cli.lk"
cp "$BODY" "$TMP/vm.lk";  echo 'route("GET","/t", function(){ return response::text(run_all()); });' >> "$TMP/vm.lk"

CLIVM=$("$LK" "$TMP/cli.lk" 2>&1)

"$FCGI" --mode http --port $PORT "$TMP/vm.lk" >"$TMP/web_main.log" 2>&1 &
SRV=$!
sleep 2
WEBVM=$(curl -s --max-time 15 "http://127.0.0.1:$PORT/t")

fail=0
# ── The guard's OWN positive check (2026-09-30) ─────────────────────────────────
# If the VM cannot run a script it must give an ERROR instead of silently falling to another path (LOOK has
# no other path). If this precondition breaks the guard goes falsely green — exactly like the body
# exceeding the 255-register limit and running for months without the VM (in the interpreter). Prove both directions:
printf 'use cache\nprint(cache::__guard_probe_not_a_fn())\n' > "$TMP/strict_probe.lk"
if "$LK" "$TMP/strict_probe.lk" >/dev/null 2>&1; then
  echo "FAIL: a call to a function that does not exist did not fail on the command line — the CLI-VM column cannot be trusted"; fail=1
fi
grep -q "^\[BYTECODE\] OK" "$TMP/web_main.log" \
  || { echo "FAIL: the web-VM column was not compiled in the VM (log: $(grep -m1 -iE 'error|fallback' "$TMP/web_main.log"))"; fail=1; }
[ -z "$CLIVM" ] && { echo "FAIL: CLI-VM gave no output (script error?)"; fail=1; }
[ "$CLIVM" = "$WEBVM" ] || { echo "FAIL: CLI-VM != web-VM"; fail=1; }

# ── FIXED REFERENCE (expected result files, tests/golden/differential/) ────────────
# Cross-column comparison cannot see a bug in the code the two columns SHARE (VM and builtin modules
# are common): both give the same wrong answer. These files pin the outputs from the moment the three engines (before the
# interpreter was removed) validated each other (2026-10-19, identical output from the engines). On an intentional change:
# rewrite with UPDATE_GOLDEN=1 and explain the diff in the commit.
# If the file is missing: FAIL (no silent green).
# Updating writes only if the columns give the SAME output: if the same name comes a
# second time in this run with a different value, FAIL (the last writer must not silently win).
GOLDEN_DIR="$DIR/golden/differential"
GOLDEN_SEEN=" "
golden() {   # golden <name> <value> <label>
  local f="$GOLDEN_DIR/$1.out"
  if [ "${UPDATE_GOLDEN:-0}" = 1 ]; then
    mkdir -p "$GOLDEN_DIR"
    case "$GOLDEN_SEEN" in
      *" $1 "*) [ "$(cat "$f")" = "$2" ] || { echo "FAIL: $3 differs from the other column — golden NOT written"; fail=1; }; return ;;
    esac
    printf '%s' "$2" > "$f"; GOLDEN_SEEN="$GOLDEN_SEEN$1 "; echo "  (golden written: $1)"; return
  fi
  if [ ! -f "$f" ]; then echo "FAIL: golden file missing: $f (create it with UPDATE_GOLDEN=1)"; fail=1; return; fi
  [ "$(cat "$f")" = "$2" ] || { echo "FAIL: $3 deviates from the fixed reference ($f)"; fail=1; }
}
golden run_all "$CLIVM" "run_all (CLI-VM)"
golden run_all "$WEBVM" "run_all (web-VM)"

# ── CLI-specific surface: print/write BYTE BY BYTE + exit code ─────────────────────
# The $(...) comparison above SWALLOWS the trailing newline and the body prints
# once → print's newline/" " separator semantics are invisible. Indeed the VM's print added neither newline nor
# separator (interpreter: args joined with " " + "\n") and this guard MISSED it.
cat > "$TMP/io.lk" <<'LK'
print("A")
write("B")
print("C")
print("x", "y", "z")
write("p", "q")
print("son")
LK
"$LK" "$TMP/io.lk" > "$TMP/io_vm.out" 2>&1
# Byte-by-byte comparison must NOT DEPEND ON AN EXTERNAL TOOL: `cmp`/`diff` (diffutils) minimal
# are ABSENT in containers — when the command is not found it returns non-zero and "bytes differ" is assumed
# (FALSE ALARM: this exactly happened, there is no cmp on AlmaLinux 8 minimal). od + $(...)
# is not safe either: $(...) swallows the trailing newline. Fix: write the od dump to a FILE,
# compare as shell strings — od is available everywhere (coreutils).
od -c < "$TMP/io_vm.out"   > "$TMP/io_vm.od"
io_vm_dump=$(cat "$TMP/io_vm.od")
golden print_write_bytes "$io_vm_dump"   "print/write bytes (CLI-VM)"
printf 'print("basla")\nexit(3)\nprint("OLMAMALI")\n' > "$TMP/ex.lk"
"$LK" "$TMP/ex.lk" >/dev/null 2>&1; v_code=$?
if [ "$v_code" != "3" ]; then
  echo "FAIL: exit(3) exit code — CLI-VM=$v_code (want 3)"
  fail=1
fi

# ── TOP-LEVEL compound assignment (+= -= *= /= %= .=) — GLOBAL scope ──────────────
# WHY SEPARATE: the differential body runs INSIDE the run_all() FUNCTION → the compound
# assignments there test the function-local path and that path was ALREADY correct.
# The bug was in the compiler's TOP-LEVEL GLOBAL branch: e.op was ignored → "$t += $i"
# was compiled as just "$t = $i". Result: interpreter CORRECT, VM SILENTLY WRONG
# (sum/string accumulation broken, no error). So the check must be TOP-LEVEL —
# adding it to the body would NOT CATCH the bug. (The surface the guard does not cover = where the bug lives.)
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
ca_vm=$("$LK" "$TMP/ca.lk" 2>&1)
ca_bek="xy|3|7|12|3|2|10"
[ "$ca_vm"   = "$ca_bek" ] || { echo "FAIL: top-level compound CLI-VM: [$ca_vm] (expected $ca_bek) — the compiler's global branch may be ignoring e.op"; fail=1; }

# ── TEMPLATE engine: must be IDENTICAL to the language semantics ──────────────────
# WHY: the template engine had COPIED truthiness and float formatting FROM THE LANGUAGE and
# diverged in both:
#   {#if "0"}      → the template TRUE, the language FALSE  (fields that come from the DB as the string "0"!)
#   {$1234567.5}   → the template "1.23457e+06" (scientific + LOSS OF PRECISION), the language "1234567.5"
# Both are SILENT: no error, only the wrong branch / wrong number. The template engine had
# NO guard AT ALL — that is why it slipped through. It now delegates to the language; this check locks it in.
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
tpl_out=$(cd "$TMP" && "$LK" tpl.lk 2>&1)
echo "$tpl_out" | grep -q 'KOD:if0:F ifs:T f:1234567.5' || { echo "FAIL: template guard — unexpected KOD line: [$tpl_out]"; fail=1; }
echo "$tpl_out" | grep -q 'if0:F ifs:T f:1234567.5' || { echo "FAIL: TEMPLATE DEVIATES from the language semantics (truthiness of \"0\" or float format): [$tpl_out]"; fail=1; }
echo "$tpl_out" | grep -q 'esc:&lt;b&gt;&amp;' || { echo "FAIL: template HTML escaping is broken (XSS risk): [$tpl_out]"; fail=1; }

# ── array::set — it DESTROYED data in a NUMERIC array ──────────────────────────────
# WHY: the "Convert numeric array to assoc" branch did not convert, it REPLACED —
# only built a brand-new assoc holding the new key/value and
# dropped the original elements. Even at a VALID index:
#   array::set([1,2,3], 1, "X") = {"1":"X"}   (expected [1,"X",3])
# Silent data loss: no error, count changes, old elements become unreachable.
# array::set was NOT in the guard AT ALL (the assoc path was correct, only it was used).
# Contract: valid index → replace in place; equal to size → append; string
# key / out of range → convert to assoc but KEEP THE EXISTING ELEMENTS.
cat > "$TMP/aset.lk" <<'LK'
use array
print(json::encode(array::set([1,2,3], 1, "X")) . "|" .
      json::encode(array::set([1,2,3], 3, "X")) . "|" .
      json::encode(array::set([1,2,3], "k", "X")) . "|" .
      json::encode(array::set([], "k", "X")) . "|" .
      json::encode(array::set(["a"=>1,"b"=>2], "a", 9)))
LK
as_vm=$("$LK" "$TMP/aset.lk" 2>&1)
as_bek='[1,"X",3]|[1,2,3,"X"]|{"0":1,"1":2,"2":3,"k":"X"}|{"k":"X"}|{"a":9,"b":2}'
[ "$as_vm"   = "$as_bek" ] || { echo "FAIL: array::set CLI-VM: [$as_vm] (expected $as_bek) — data may be destroyed in a numeric array"; fail=1; }

# ── $arr["key"] — string key on a NUMERIC list ─────────────────────────────────
# WHY: both engines were wrong, in different ways (this case was NOT in the guard AT ALL):
#   VM        : treated the list as a pair list and prepended a sentinel ->
#               $a=[1,2,3]; $a["k"]="X"  =>  {"1":2,"3":"k"}  ALL DATA IS DESTROYED,
#               even the written value is unreachable ($a["k"] -> null)
#   tree-walk : did not enter the assoc branch but fell into the numeric branch, to_int("k")=0 ->
#               ["X",2,3]  the wrong element is overwritten, the key vanishes
# Both are SILENT. Contract: convert the list to assoc but KEEP THE EXISTING ELEMENTS
# by keying them with their numeric indexes (same as array::set).
# Also, an INTEGER string such as "1" is a numeric index, not an assoc key —
# it used to be: the VM converted to assoc and the tree-walk wrote to the index (divergence).
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
# NUMERIC index edges — the VM was aligned to tree-walk (the reference semantics).
# OLD BUG: the VM was SILENT for out-of-range and negative, while the interpreter
# threw an error / counted from the end — the same program gave two different results on two engines:
#   read $a[99]    -> VM null / tree-walk ERROR
#   read $a[-1]    -> VM null / tree-walk 3
#   $a[99]="X"    -> VM SILENTLY SKIPS the assignment / tree-walk ERROR
#   $a[-1]="X"    -> VM SILENTLY SKIPS the assignment / tree-walk [1,2,"X"]
# "I wrote it but it was not written" and "I read it, got null (though it should have been an error)" are the same
# class: silently wrong data. Contract: negative = from the end, out of range = ERROR,
# writing at index == size = append.
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
in_vm=$("$LK" "$TMP/inum.lk" 2>&1)
[ "$in_vm"   = "$in_bek" ] || { echo "FAIL: \$arr[number] edges CLI-VM: [$in_vm] (expected $in_bek) — the VM may be SILENT out of range / on negative indexes"; fail=1; }

ik_vm=$("$LK" "$TMP/ikey.lk" 2>&1)
ik_bek='{"0":1,"1":2,"2":3,"k":"X"}|X|2|{"k":"X"}|[1,"X",3]|null|2'
[ "$ik_vm"   = "$ik_bek" ] || { echo "FAIL: \$arr[str] CLI-VM: [$ik_vm] (expected $ik_bek) — elements may not be preserved when a list is converted to an assoc, or a string key may be forced into a numeric index"; fail=1; }


# ── string::replace — an EMPTY search string used to HANG (DoS) ───────────────────────────
# WHY: s.find("", pos) always returns pos -> every round the loop inserts 'to' between
# characters, the string keeps growing: INFINITE LOOP + UNBOUNDED MEMORY. On the web
# string::replace($text, $userInput, $x) call, if the user sends an empty string
# the worker locked up forever — a DoS with a single request.
# Contract: Go (strings.Replace(s,"",new,-1)) — an empty search string matches at the START of the text
# and matches AFTER every UTF-8 sequence, k+1 insertions for k code points.
# Run with a TIMEOUT: a hang regression should show up as a FAIL (not silently
# waiting and locking up CI).
cat > "$TMP/rep.lk" <<'LK'
use string
print(string::replace("abc", "", "X") . "|" . string::replace("abc", "", "") . "|" .
      string::replace("şğ", "", "-") . "|" . string::replace("", "", "X") . "|" .
      string::replace("aXbXc", "X", "-"))
LK
rp_bek='XaXbXcX|abc|-ş-ğ-|X|a-b-c'
rp_vm=$(timeout 10 "$LK" "$TMP/rep.lk" 2>&1); rp_rc2=$?
[ $rp_rc2 -eq 124 ] && { echo "FAIL: string::replace CLI-VM HUNG (an empty search string loops forever)"; fail=1; }
[ $rp_rc2 -eq 124 ] || [ "$rp_vm" = "$rp_bek" ] || { echo "FAIL: string::replace CLI-VM: [$rp_vm] (expected $rp_bek)"; fail=1; }


# ── string::pad_* — it PRODUCED INVALID UTF-8 + byte/code-point inconsistency ───
# WHY (three bugs from a single root):
#   1) TRUNCATED long text: pad_left("abcdef",3,"0") = "def" (silent data loss;
#      PHP str_pad / Python ljust / Go fmt do not truncate — width is a MINIMUM)
#   2) Truncation was BYTE-based -> it split a multi-byte character in the middle:
#      pad_left("şğü",3,"x") = 0x9F 0xC3 0xBC — starts with an orphan continuation byte, i.e.
#      INVALID UTF-8. The corrupt text went as-is into JSON/DB/HTTP responses.
#   3) Width counted BYTES, whereas len/substr/upper/reverse count code points:
#      pad_left("ş",3,"x") = "xş", visible length 2 (expected 3) — the module was inconsistent
#      within itself.
# Contract: Go fmt "%Ns" — width is a MINIMUM and in CODE POINTS. No truncation.
# The guard also checks whether the output is valid UTF-8 (iconv) — a direct lock for the 2nd bug.
cat > "$TMP/pad.lk" <<'LK'
use string
print(string::pad_left("ş",3,"x") . "|" . string::len(string::pad_left("ş",3,"x")) . "|" .
      string::pad_right("ş",3,"x") . "|" . string::pad_left("a",4,"ş") . "|" .
      string::pad_left("abcdef",3,"0") . "|" . string::pad_left("şğü",3,"x") . "|" .
      string::pad_left("",3,"x") . "|" . string::pad_left("ab",0,"x"))
LK
pd_bek='xxş|3|şxx|şşşa|abcdef|şğü|xxx|ab'
pd_vm=$(timeout 10 "$LK" "$TMP/pad.lk" 2>&1)
[ "$pd_vm"   = "$pd_bek" ] || { echo "FAIL: string::pad_* CLI-VM: [$pd_vm] (expected $pd_bek) — byte-based width or truncation may be back"; fail=1; }
if command -v iconv >/dev/null 2>&1; then
  printf '%s' "$pd_vm" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 || { echo "FAIL: string::pad_* produces INVALID UTF-8 (a multi-byte character is split in the middle)"; fail=1; }
fi


# ── string::upper/lower — the "global" claim + there is NO Turkish locale ─────────────
# TWO-WAY LOCK:
# 1) NO TURKISH LOCALE (the language is global): upper("i") = "I" must hold, NOT "İ".
#    ı → I, İ → i (standard Unicode). This is a deliberate decision — the general
#    even/odd rule of Latin Ext-A gives ı(0x131) -> İ(0x130), so if the rule is
#    written in the wrong order the Turkish locale SILENTLY comes back. This check catches it.
# 2) is the COVERAGE really global: the table used to consist of ASCII + Latin-1 + 3 Turkish code
#    points; Cyrillic/Greek/Latin Ext-A passed through silently unconverted
#    (upper("привет") = "привет", upper("łódź") = "łÓDź" — HALF conversion).
cat > "$TMP/case.lk" <<'LK'
use string
print(string::upper("i") . string::lower("I") . string::upper("ı") . string::lower("İ") . "|" .
      string::upper("çğöşü") . "|" . string::upper("привет") . "|" . string::lower("ПРИВЕТ") . "|" .
      string::upper("ελλάδα") . "|" . string::upper("łódź") . "|" . string::upper("čeština") . "|" .
      string::lower(string::upper("привет")))
LK
cs_bek='IiIi|ÇĞÖŞÜ|ПРИВЕТ|привет|ΕΛΛΆΔΑ|ŁÓDŹ|ČEŠTINA|привет'
cs_vm=$(timeout 10 "$LK" "$TMP/case.lk" 2>&1)
[ "$cs_vm"   = "$cs_bek" ] || { echo "FAIL: string::upper/lower CLI-VM: [$cs_vm] (expected $cs_bek) — the Turkish locale may be back or coverage may be incomplete"; fail=1; }


# ── SECURITY: can request::ip() be spoofed? ────────────────────────────────────
# WHY: ctx.remote_addr was filled only from the X-Forwarded-For header and
# there was NO trusted-proxy check. The client picked its own IP:
#   curl -H "X-Forwarded-For: 9.9.9.9"  ->  request::ip() = "9.9.9.9"
# IP-based authorization, the ban list and rate-limit could be bypassed.
# It also returned "" when there was no header at all (the real peer was ignored).
# Contract: the REAL TCP peer is authoritative; X-Real-IP / X-Forwarded-For are consulted only if the
# connecting IP is in the LOOK_TRUSTED_PROXY list; the FIRST of the chain is taken.
# This check runs WITHOUT LOOK_TRUSTED_PROXY: whatever header is sent,
# the result must be the real peer.
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
[ "$ip_plain" = "127.0.0.1" ] || { echo "FAIL: request::ip() does not return the real peer when no header is present: [$ip_plain]"; fail=1; }
[ "$ip_spoof" = "127.0.0.1" ] || { echo "FAIL: SECURITY — request::ip() CAN BE SPOOFED: X-Forwarded-For made it return [$ip_spoof] (with no trusted proxy configured it must be the real peer)"; fail=1; }
[ "$ip_chain" = "127.0.0.1" ] || { echo "FAIL: SECURITY — request::ip() trusts the XFF chain: [$ip_chain]"; fail=1; }
[ "$ip_real"  = "127.0.0.1" ] || { echo "FAIL: SECURITY — request::ip() trusts X-Real-IP: [$ip_real]"; fail=1; }


# ── SECURITY: can session data be injected? ───────────────────────────
# WHY: the session blob is in a line-based "key=value\n" format and keys/values were
# NOT ESCAPED AT ALL -> a \n inside a value defined A NEW FIELD.
# If the app writes user data into the session (name/note/search — very common):
#   ?isim=bob\nrol=admin\nadmin=1  ->  session::get("admin") = "1"
# an authorization field that DID NOT EXIST AT ALL was injected (even in a pre-login request).
# Contract: store escaped — a legitimate line end is preserved as DATA, it carries
# no separator meaning. The guard checks both the attack and the round trip.
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
[ "$ss_out" = '"uye"|null' ] || { echo "FAIL: SECURITY — session data CAN BE INJECTED: [$ss_out] (expected \"uye\"|null; a newline inside a value may define a new field)"; fail=1; }


# ── SECURITY: cookie::set attribute injection ─────────────────────────────────
# WHY: sanitize_header() cleaned only CR/LF/NUL; in a cookie ';' is a STRUCTURAL
# SEPARATOR, so a value under user control could inject an attribute:
#   ?v=x; HttpOnly; Domain=evil.com
#   -> Set-Cookie: tercih=x; HttpOnly; Domain=evil.com; Path=/
# It was possible to pull the Domain up to the parent domain and leak the cookie to all subdomains.
# Contract: like PHP setcookie(), percent-encode on write, decode on read. A legitimate
# value is preserved but structure cannot be carried.
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
# NOTE: do NOT search for the word in the whole header — the encoded value contains the text "HttpOnly"
# as plain text (x%3B%20HttpOnly%3B%20...) and that is SAFE. Injection
# happens only with a REAL ';' separator, so search for the ATTRIBUTE FORM.
# NOTE (2026-08 flip): cookie::set now adds the DEFAULT "; HttpOnly; SameSite=Lax" →
# the "; HttpOnly" canary collides with the legitimate default, cannot be used. Domain is the INJECTION canary
# (below) remains (Domain is NOT a default → its presence = injection). For HttpOnly: POSITIVELY verify that the value was
# ACTUALLY encoded (the ';' inside the value must be %3B).
echo "$ck_hdr" | grep -qi "; *Domain=" && { echo "FAIL: SECURITY — cookie::set ATTRIBUTES CAN BE INJECTED: [$ck_hdr]"; fail=1; }
echo "$ck_hdr" | grep -qi "tercih=x%3b" || { echo "FAIL: SECURITY — cookie value ';' was NOT ENCODED (injection is open): [$ck_hdr]"; fail=1; }
[ "$ck_rt" = '"a;b"' ] || { echo "FAIL: cookie round trip is broken: [$ck_rt] (expected \"a;b\" — there may be encoding but no decoding)"; fail=1; }


# ── db:: parameter binding — '?' inside a literal + count mismatch ───────────────
# WHY: bind_params blindly replaced ALL '?' characters; string
# '?' inside literals/identifiers/comments was also taken as a placeholder:
#   "SELECT 'Hazir mi?' AS s, ? AS p" + ["DEGER"]
#   -> SELECT 'Hazir mi'DEGER'' AS s, ? AS p   (literal broken, param shifted)
# ANY text containing a question mark could not be used in a SQL literal
# (very common in Turkish sentences; in PostgreSQL '?' is also a JSONB operator).
# Parameter count mismatch was also SILENT IN BOTH DIRECTIONS: extra params were dropped,
# missing params left '?' and SQLite took it as its own placeholder and gave NULL.
# Contract: '?' is a placeholder only in the SQL BODY; count mismatch is an ERROR (in Go
# db.Query also returns an error). The SQL INJECTION check is also locked in here.
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
[ "$db_out" = "$db_bek" ] || { echo "FAIL: db:: parameter binding: [$db_out] (expected $db_bek) — a '?' inside a literal may be taken as a placeholder, or a count mismatch may be SILENT"; fail=1; }


# ── ERROR MESSAGE parity — when 'use' is forgotten ───────────────────────────────
# WHY: if the module was not 'use'd, the VM builtin table entry is an EMPTY std::function;
# when called, C++ throws std::bad_function_call and it reaches the user AS IS
# it came out like this:
#   print(string::len("abc"))  ->  "Runtime Error: bad_function_call"
# It names neither the module nor the function, and it is a C++ internal term. tree-walk (REFERENCE)
# gave the right message: "Module 'string' not loaded."
# Forgetting 'use' is THE MOST COMMON mistake in LOOK, so the DEFAULT engine gave the worst message.
# The error text is also part of the contract (S3): the fixed reference (golden) locks it.
cat > "$TMP/uerr.lk" <<'LK'
print(string::len("abc"))
LK
cat > "$TMP/uok.lk" <<'LK'
use string
print(string::len("abc"))
LK
ue_vm=$("$LK" "$TMP/uerr.lk" 2>&1 | grep -i "error" | head -1)
uo_vm=$("$LK" "$TMP/uok.lk" 2>&1 | tail -1)
echo "$ue_vm" | grep -q "bad_function_call" && { echo "FAIL: a forgotten 'use' leaks a VM C++ internal exception: [$ue_vm]"; fail=1; }
golden use_missing_error "$ue_vm"   "error message when 'use' is forgotten (CLI-VM)"
[ "$uo_vm" = "3" ] || { echo "FAIL: module call broke WHEN 'use' IS PRESENT (positive control): [$uo_vm]"; fail=1; }

# The NAME that cannot be called — the message must say the name.
# OLD BUG (both directions): the VM did not know the name ("Not callable (BYTECODE_FN
# expected)" — which name?); tree-walk always said "is not defined",
# yet the value may be DEFINED and merely not callable ($x = 5; $x(1)).
# So the REFERENCE ENGINE was wrong too. The name is embedded by the compiler into a NOP hint
# and carried into the VM (the 16-bit index pattern of CALL_BUILTIN).
printf 'print(olmayan_fonksiyon(1))\n' > "$TMP/c1.lk"
printf '$x = 5\nprint($x(1))\n'        > "$TMP/c2.lk"
for cf in c1 c2; do
  cv=$("$LK" "$TMP/$cf.lk" 2>&1 | grep -i error | head -1)
  echo "$cv" | grep -q "BYTECODE_FN" && { echo "FAIL: call error leaks an internal term (the name is not reported): [$cv]"; fail=1; }
done
echo "$("$LK" "$TMP/c2.lk" 2>&1)" | grep -q "is not defined" && {
  echo "FAIL: misleading 'is not defined' message for a value that is defined but not callable"; fail=1; }


# ── PostgreSQL wire — does a CORRUPT DataRow silently produce wrong data? ──────────
# WHY: postgres_client.cpp (1023 lines) had NO guard AT ALL. Measured
# with a fake server — on a corrupt DataRow LOOK silently returned wrong data:
#   says length 100, sends 2 bytes   -> field "" (empty string), NO ERROR
#   int32-max length                 -> field "" , NO ERROR
#   promises 3 fields, sends 1       -> row returns with 1 column, NO ERROR
# There was no crash (the bounds check held) but the app took the empty value
# as real data and acted on it. Silent wrong data is more dangerous than a crash in this project.
# Also 'p + field_len <= end' was open to pointer OVERFLOW.
# Contract: corrupt DataRow -> CLEAR ERROR; a valid row works unchanged.
if command -v python3 >/dev/null 2>&1 && [ -f "$(dirname "$0")/fake_pg_server.py" ]; then
  FPG="$(dirname "$0")/fake_pg_server.py"
  cat > "$TMP/pg.lk" <<'LK'
try {
  $c = db::connect(env("PGDSN", ""))
  print("SONUC=" . json::encode(db::query($c, "SELECT col FROM t")))
} catch ($e) { print("HATA") }
LK
  pg_sonuc=""
  # Ports are chosen OUTSIDE the ephemeral range and separately for each case. It used to
  # pick randomly from 55700-55899: that range is also used as the source port of outgoing
  # connections, the fake server occasionally failed to open with "Address already in use" and
  # the healthy case returned ERROR (4 in 40 tries locally).
  pgbase=$((21700 + RANDOM % 190)); pgi=0
  for mod in saglam truncated negatif dev_uzunluk eksik_alan; do
    pgport=$((pgbase + pgi)); pgi=$((pgi + 1))
    python3 "$FPG" $mod $pgport > "$TMP/fpg.log" 2>&1 &
    fpgpid=$!
    for i in $(seq 1 25); do grep -q hazir "$TMP/fpg.log" 2>/dev/null && break; sleep 0.2; done
    out=$(PGDSN="postgres://u:p@127.0.0.1:$pgport/test" timeout 15 "$LK" "$TMP/pg.lk" 2>&1 | grep -E '^(SONUC|HATA)' | head -1)
    kill $fpgpid 2>/dev/null; wait $fpgpid 2>/dev/null
    pg_sonuc="$pg_sonuc${out%%=*}|"
  done
  # healthy=SONUC, ALL the others must give an error
  pg_bek='SONUC|HATA|HATA|HATA|HATA|'
  [ "$pg_sonuc" = "$pg_bek" ] || { echo "FAIL: PostgreSQL wire — a malformed DataRow may silently return data: [$pg_sonuc] (expected $pg_bek)"; fail=1; }
else
  echo "  (skipped: the PostgreSQL wire test needs python3)"
fi


# ── MODE PARITY: does multipart upload also work in --mode http? ─────────────
# WHY: body parsing was written in THREE separate places and the copy in http_main
# there was NO multipart branch AT ALL -> request::file() works in FastCGI, in --mode http it
# gave a "requires multipart/form-data request" error. The same app behaved
# differently by mode (S12). The README listed it as a "known limit"; but it was
# a forgotten branch. Also http_main imposed a method gate (POST-only),
# FastCGI did not -> PUT/PATCH bodies were also parsed differently by mode.
# Contract: body parsing from a SINGLE source (WebContext::parse_post_body).
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
[ "$mp_out" = "OK|23|merhaba" ] || { echo "FAIL: --mode http: multipart/request::file does not work: [$mp_out] (expected OK|23|merhaba — 23 = "multipart test content" + newline)"; fail=1; }
[ "$put_out" = '"putdan"' ] || { echo "FAIL: --mode http: the PUT body is not parsed (no parity with FastCGI): [$put_out]"; fail=1; }


# ── http:: SSRF + IPv6 URL parsing ────────────────────────────────────────
# TWO THINGS ARE LOCKED:
# 1) the SSRF BLOCK must be distinguishable from a REAL NETWORK ERROR. tcp_connect returns the same INVALID for a DNS error,
#    an SSRF block and a connection error, and the caller
#    said "connection failed" for all of them -> a developer who could not reach an internal service
#    could not see that a SECURITY POLICY had blocked it.
# 2) IPv6 literal URLs DID NOT WORK AT ALL: parse_url did not strip the square brackets,
#    looking for the port separator with rfind(':') ->
#      "[::1]:8080" -> host "[::1]"  (bracketed, getaddrinfo fails)
#      "[::1]"      -> host "[:"     (the colon INSIDE the address was taken as a port)
#    Since the error was "DNS could not be resolved" the cause was not visible; only once (1) was done and
#    the messages were told apart did it surface.
# This check DOES NOT NEED A SERVER: with SSRF protection ON the expected error for [::1]
# is SSRF. If parsing breaks again the error returns to "DNS could not be resolved" and it FAILs.
cat > "$TMP/ssrf.lk" <<'LK'
use http
$a = http::get("http://[::1]:9/")
$b = http::get("http://127.0.0.1:9/")
$r = http::get("http://[::1:9/")
$c = $r.error
print($a.error . "|" . $b.error . "|" . $c)
LK
sf_out=$(timeout 30 "$LK" "$TMP/ssrf.lk" 2>&1 | tail -1)
# THREE SEPARATE conditions instead of a single regex — in grep '|' is a plain character, expecting
# alternation gives a false alarm (this exact mistake happened while writing this check).
sf_ok=1
if echo "$sf_out" | grep -q "DNS resolution failed"; then
  echo "FAIL: IPv6 literal URL parsing is broken — a DNS error is returned for [::1],"
  echo "      so the square brackets are not stripped (the host goes as \"[::1]\")"
  sf_ok=0
fi
if echo "$sf_out" | grep -q "connection failed"; then
  echo "FAIL: SECURITY DIAGNOSIS — the SSRF block cannot be told apart from a real network error"
  sf_ok=0
fi
echo "$sf_out" | grep -qi "ipv6" || {
  echo "FAIL: a malformed IPv6 URL ('[::1:9/') does not give a clear error"; sf_ok=0; }
[ $sf_ok = 1 ] || { echo "  output: [$sf_out]"; fail=1; }


# ── Is the USER MODULE (`use`) also loaded in the VM? ───────────────────────────
# WHY: `use <name>` covers two things — a stdlib module (already bound
# in the builtin table) and a USER MODULE (~/.look/modules/<name>/<name>.lk). The compiler
# produced a NOP for both; the user module's functions never appeared in the VM globals,
# the route gave "Undefined variable: <fn>" and fell PERMANENTLY to the interpreter.
# It stayed silent because the output was CORRECT — the fallback masks the bug (rule 1).
# I.e. the language's "multi-file project via use" path was LOSING ITS SPEED on the web.
# This check verifies both the output and "no VM BUG".
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
[ "$um_out" = "um=42" ] || { echo "FAIL: the user module does not work on the web: [$um_out] (expected um=42)"; fail=1; }
if grep -q "VM BUG" "$TMP/um.log" 2>/dev/null; then
  echo "FAIL: the user module does not load in the VM — the route PERMANENTLY fell back to the interpreter (slow path):"
  grep -m1 "VM BUG" "$TMP/um.log" | sed 's/^/       /'
  fail=1
fi


# ── LEXER — resilience to corrupt/malicious source ─────────────────────────────
# The language's input gate; it had no guard. It was scanned, no serious bug was FOUND — this check locks
# the behaviors that turned out sound against REGRESSION:
#   unterminated -> error (not an endless read), deep nesting -> parser guard (not a stack
#   overflow), unicode surrogate -> correct (a lone surrogate is safe, a pair
#   combines). All in a single column (the lexer is shared) — two columns not needed.
lx_fail=0
printf 'print("acik' > "$TMP/lx_str.lk"
"$LK" --check "$TMP/lx_str.lk" 2>&1 | grep -qi "Unterminated" || { echo "FAIL: the lexer does not catch an unterminated string"; lx_fail=1; }
# deep nesting — not a stack overflow, a clear error
python3 -c 'open("'"$TMP"'/lx_deep.lk","w").write("print(" + "("*20000 + "1" + ")"*20000 + ")")' 2>/dev/null && {
  dc=$(timeout 10 "$LK" --check "$TMP/lx_deep.lk" 2>&1 | head -1)
  echo "$dc" | grep -qi "derin\|deep\|max" || { echo "FAIL: no deep-nesting parser guard: [$dc]"; lx_fail=1; }
}
# unicode surrogate pair -> a single code point
printf 'use string\nprint(string::len("\\uD83D\\uDE00"))' > "$TMP/lx_emoji.lk"
em=$("$LK" "$TMP/lx_emoji.lk" 2>&1 | tail -1)
[ "$em" = "1" ] || { echo "FAIL: a surrogate pair does not combine into one code point: [$em] (expected 1)"; lx_fail=1; }
[ $lx_fail = 0 ] || fail=1


# ── JSON integer PRECISION beyond int64 (bug 52) ───────────────────────────────
# json::decode silently dropped an integer beyond int64 to float → the ID was silently
# corrupted (9223372036854775809 -> ...808). Precision must be preserved: it must come back
# exactly as a string. json_decode_value is a single source (stdlib) — testing on one engine is enough.
printf 'use json\n$d = json::decode("{\\"id\\": 9223372036854775809, \\"big\\": 18446744073709551615}")\nprint($d["id"])\nprint($d["big"])' > "$TMP/js_int.lk"
ji=$("$LK" "$TMP/js_int.lk" 2>&1)
echo "$ji" | grep -q "9223372036854775809" || { echo "FAIL: json id above int64 LOST PRECISION (expected 9223372036854775809): [$ji]"; fail=1; }
echo "$ji" | grep -q "18446744073709551615" || { echo "FAIL: json uint64-max LOST PRECISION (expected 18446744073709551615): [$ji]"; fail=1; }


# ── Is the dispatch copy REUSED in the worker, is isolation broken? ──
# The copy is now thread_local (the per-request make_dispatch_copy took 30-63us — more expensive than
# running the route). Contract: the shared copy must not LEAK between requests.
#   $s = $counter + 1  → function-local (1 per request)  — assignment is isolated
#   ($counter = $counter + 1 is now a load-time error: a name read from outside cannot be assigned in the same function —
#    tests/outer_read_assign_test.sh)
# These values must be IDENTICAL to those before the thread_local optimization.
cat > "$TMP/iso.lk" <<'LK'
$sayac = 0
route("GET","/inc", function(){ $s = $sayac + 1; return response::text("s=" . $s) })
LK
ISO_PORT=9613
"$FCGI" --mode http --port $ISO_PORT "$TMP/iso.lk" >/dev/null 2>&1 &
IS=$!
sleep 2
iso_all=""
for i in 1 2 3 4 5; do iso_all="$iso_all$(curl -s --max-time 8 "http://127.0.0.1:$ISO_PORT/inc")"; done
kill $IS 2>/dev/null; sleep 1; kill -9 $IS 2>/dev/null
[ "$iso_all" = "s=1s=1s=1s=1s=1" ] || { echo "FAIL: dispatch-copy isolation is BROKEN (global leak): [$iso_all] (expected s=1 x5)"; fail=1; }

# ── ODR: client/server type clash (CRASH with LTO) ───────────────────────────
# look::HttpResponse was defined DIFFERENTLY in BOTH http_client.h AND http_server.h (same
# namespace!). look-fcgi links both → UNDEFINED BEHAVIOR; with LTO on, the two
# layouts are taken as a single type and merged → an object built with one layout and
# is destroyed with the other → "free(): invalid pointer"/segfault. Calling http::get from a web route
# CRASHED the server (a single request was enough). It was absent in the CLI (http_server.cpp is not
# linked), absent in ASan (LTO is off in that build) — so it went unseen for months.
# Contract: http::get must work in a web route and the server must STAY UP.
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
[ "$odr_r" = '{"st":200}' ] || { echo "FAIL: http::get is broken in a web route: [$odr_r] (ODR/crash?)"; fail=1; }
[ "$odr_alive" = "yes" ] || { echo "FAIL: the SERVER CRASHED after http::get (is the look::HttpResponse ODR violation back?)"; fail=1; }
if [ $fail -eq 0 ]; then
  echo "PASS: CLI-VM == web-VM == golden (2 columns x 22 categories + CLI print/exit + no silent fallback)"
  echo "  $CLIVM"
  exit 0
else
  echo "  CLI-VM   : $CLIVM"
  echo "  web-VM   : $WEBVM"
  exit 1
fi
